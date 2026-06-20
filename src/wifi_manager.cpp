// wifi_manager.cpp
// ─────────────────────────────────────────────────────────────────────────────
// Non-blocking WiFi state machine.
//
// All WiFi activity is controlled by a single WifiState enum. Only one process
// (connecting, scanning, etc.) runs at a time. Transitions are explicit; no
// two states may be active simultaneously.
//
// State diagram:
//
//   IDLE ──(creds exist + power ON)──────────────► CONNECTING
//        ──(scan request)──────────────────────────► SCAN_PREPARE
//
//   CONNECTING ──(WL_CONNECTED)──────────────────► CONNECTED
//              ──(failed / timeout)──────────────► FAILED
//              ──(scan request)──────────────────► SCAN_PREPARE
//
//   CONNECTED ──(WiFi drops)──────────────────────► CONNECTING  (auto-retry)
//             ──(scan request)──────────────────────► SCAN_PREPARE
//
//   FAILED ──(user reconnect)────────────────────► CONNECTING
//          ──(scan request)──────────────────────► SCAN_PREPARE
//
//   SCAN_PREPARE ──(radio settled ≥1500ms)────────► SCAN_RUNNING
//
//   SCAN_RUNNING ──(scan complete / error)────────► SCAN_DONE
//               ──(timeout 15s)────────────────────► SCAN_DONE
//
//   SCAN_DONE ──(restore_needed)──────────────────► CONNECTING
//             ──(no restore)────────────────────────► IDLE
// ─────────────────────────────────────────────────────────────────────────────

#include "wifi_manager.h"
#include "data.h"
#include "mqtt_manager.h"

// ─── Module-level state ───────────────────────────────────────────────────────
static Preferences   prefs;
static WifiState     wifi_state             = WIFI_STATE_IDLE;
static bool          wifi_power_policy_on   = true;
static bool          wifi_restore_after_scan = false;   // reconnect after scan completes?
static uint32_t      wifi_state_entered_ms  = 0;        // timestamp of state entry
static uint32_t      wifi_scan_prepare_ms   = 0;        // timestamp of SCAN_PREPARE entry

// ─── Timing constants ─────────────────────────────────────────────────────────
static const uint32_t WIFI_CONNECT_TIMEOUT_MS  = 15000;  // 15 s
static const uint32_t WIFI_SCAN_TIMEOUT_MS     = 15000;  // 15 s
static const uint32_t WIFI_SCAN_SETTLE_MS      = 1500;   // radio settle after disconnect

// ─── Helpers ──────────────────────────────────────────────────────────────────

static void wifi_set_state(WifiState next) {
    if (wifi_state != next) {
        Serial.printf("[WIFI] State: %u → %u\n", (uint8_t)wifi_state, (uint8_t)next);
    }
    wifi_state = next;
    wifi_state_entered_ms = millis();
}

// Set wifi_status_detail and wifi_connected inside g_state (takes+releases lock).
static void wifi_push_status(const char* detail, bool connected) {
    data_lock(g_state);
    strncpy(g_state.net.wifi_status_detail, detail, sizeof(g_state.net.wifi_status_detail) - 1);
    g_state.net.wifi_status_detail[sizeof(g_state.net.wifi_status_detail) - 1] = '\0';
    g_state.net.wifi_connected = connected;
    g_state.ui_needs_update    = true;
    data_unlock(g_state);
}

// Clear all scan flags in g_state. Must NOT be called while holding the lock.
static void wifi_clear_scan_flags() {
    data_lock(g_state);
    g_state.net.wifi_scan_requested    = false;
    g_state.net.wifi_scan_active       = false;
    g_state.net.wifi_scan_start_pending = false;
    g_state.net.wifi_scan_radio_warming = false;
    data_unlock(g_state);
}

// Initiate a WiFi.begin() using NVS credentials and transition to CONNECTING.
// Returns false if no credentials are stored.
static bool wifi_do_begin() {
    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    if (ssid.length() == 0) {
        wifi_push_status("No saved WiFi credentials", false);
        wifi_set_state(WIFI_STATE_IDLE);
        return false;
    }
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);   // We manage reconnect ourselves
    WiFi.begin(ssid.c_str(), pass.c_str());
    wifi_push_status("CONNECTING...", false);
    wifi_set_state(WIFI_STATE_CONNECTING);
    Serial.printf("[WIFI] WiFi.begin → SSID: %s\n", ssid.c_str());
    return true;
}

// Enter SCAN_PREPARE: pause any ongoing connection, arm the radio-settle timer.
// restore = true means we will restore the saved connection after scan completes.
static void wifi_enter_scan_prepare(bool restore) {
    // Signal MQTT that the connection is going away
    if (wifi_state == WIFI_STATE_CONNECTED) {
        mqtt_request_reconnect();
    }
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);  // disconnect STA without powering off RF
    WiFi.scanDelete();

    wifi_restore_after_scan = restore;
    wifi_scan_prepare_ms    = millis();

    data_lock(g_state);
    g_state.net.wifi_scan_requested     = false;
    g_state.net.wifi_scan_active        = false;
    g_state.net.wifi_scan_start_pending = true;
    g_state.net.wifi_scan_radio_warming = true;
    g_state.net.wifi_connected          = false;
    if (wifi_state == WIFI_STATE_CONNECTED) {
        g_state.net.mqtt_ok = false;
    }
    strncpy(g_state.net.wifi_scan_status,
            restore ? "Pausing WiFi for scan..." : "Preparing radio...",
            sizeof(g_state.net.wifi_scan_status) - 1);
    g_state.net.wifi_scan_status[sizeof(g_state.net.wifi_scan_status) - 1] = '\0';
    g_state.ui_needs_update = true;
    data_unlock(g_state);

    wifi_set_state(WIFI_STATE_SCAN_PREPARE);
    Serial.printf("[SCAN] → SCAN_PREPARE (restore=%s)\n", restore ? "yes" : "no");
}

// ─── State handlers ───────────────────────────────────────────────────────────

static void handle_idle() {
    // Only react to a scan request — auto-connect happened at boot.
    data_lock(g_state);
    bool scan_req = g_state.net.wifi_scan_requested;
    data_unlock(g_state);

    if (scan_req) {
        // No saved connection to restore; just scan then return to IDLE.
        wifi_enter_scan_prepare(false);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
static void handle_connecting() {
    // Scan preempts connecting.
    data_lock(g_state);
    bool scan_req = g_state.net.wifi_scan_requested;
    data_unlock(g_state);

    if (scan_req) {
        bool has_creds = (prefs.getString("ssid", "").length() > 0);
        wifi_enter_scan_prepare(has_creds);
        return;
    }

    wl_status_t status  = WiFi.status();
    uint32_t    elapsed = millis() - wifi_state_entered_ms;

    if (status == WL_CONNECTED) {
        // ── CONNECTED ──
        data_lock(g_state);
        strncpy(g_state.net.connected_wifi_ssid, WiFi.SSID().c_str(),
                sizeof(g_state.net.connected_wifi_ssid) - 1);
        g_state.net.connected_wifi_ssid[sizeof(g_state.net.connected_wifi_ssid) - 1] = '\0';
        strncpy(g_state.net.wifi_status_detail, "Connected",
                sizeof(g_state.net.wifi_status_detail) - 1);
        g_state.net.wifi_status_detail[sizeof(g_state.net.wifi_status_detail) - 1] = '\0';
        g_state.net.wifi_connected = true;
        g_state.ui_needs_update    = true;
        data_unlock(g_state);
        wifi_set_state(WIFI_STATE_CONNECTED);
        Serial.printf("[WIFI] ✓ Connected → %s  IP: %s\n",
                      WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
        return;
    }

    if (status == WL_CONNECT_FAILED) {
        wifi_push_status("FAILED: Wrong credentials?", false);
        wifi_set_state(WIFI_STATE_FAILED);
        Serial.println("[WIFI] Connect failed: wrong credentials");
        return;
    }

    if (status == WL_NO_SSID_AVAIL) {
        wifi_push_status("FAILED: SSID not found", false);
        wifi_set_state(WIFI_STATE_FAILED);
        Serial.println("[WIFI] Connect failed: SSID not found");
        return;
    }

    if (elapsed > WIFI_CONNECT_TIMEOUT_MS) {
        wifi_push_status("FAILED: Connection timeout", false);
        wifi_set_state(WIFI_STATE_FAILED);
        Serial.println("[WIFI] Connect timeout");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
static void handle_connected() {
    // Scan preempts connected state.
    data_lock(g_state);
    bool scan_req = g_state.net.wifi_scan_requested;
    data_unlock(g_state);

    if (scan_req) {
        bool has_creds = (prefs.getString("ssid", "").length() > 0);
        wifi_enter_scan_prepare(has_creds);
        return;
    }

    // Monitor for drop, checked every 2 s.
    static uint32_t last_check_ms = 0;
    if (millis() - last_check_ms < 2000) return;
    last_check_ms = millis();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WIFI] Connection dropped — auto-retrying...");
        data_lock(g_state);
        g_state.net.wifi_connected = false;
        strcpy(g_state.net.connected_wifi_ssid, "-");
        g_state.ui_needs_update = true;
        data_unlock(g_state);
        wifi_do_begin();   // → CONNECTING or IDLE (if no creds)
    }
}

// ─────────────────────────────────────────────────────────────────────────────
static void handle_failed() {
    // Only react to a scan request; reconnect happens via wifi_manager_reconnect().
    data_lock(g_state);
    bool scan_req = g_state.net.wifi_scan_requested;
    data_unlock(g_state);

    if (scan_req) {
        // Scan from FAILED: do NOT restore connection after scan — user should
        // manually pick a network from scan results.
        wifi_enter_scan_prepare(false);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
static void handle_scan_prepare() {
    uint32_t elapsed = millis() - wifi_scan_prepare_ms;

    // Wait at least WIFI_SCAN_SETTLE_MS after disconnect for the RF driver to settle.
    if (elapsed < WIFI_SCAN_SETTLE_MS) return;

    // If STA is somehow still associated, wait up to 3 s total then proceed anyway.
    if (WiFi.status() == WL_CONNECTED && elapsed < 3000) return;

    // Radio is ready — kick off an async scan.
    WiFi.scanDelete();
    int result = WiFi.scanNetworks(true, true);

    if (result == WIFI_SCAN_RUNNING) {
        // ── Scan started ──
        data_lock(g_state);
        g_state.net.wifi_scan_start_pending  = false;
        g_state.net.wifi_scan_radio_warming  = false;
        g_state.net.wifi_scan_active         = true;
        g_state.net.wifi_scan_started_ts     = millis();
        g_state.net.wifi_scan_start_attempts++;
        strncpy(g_state.net.wifi_scan_status, "Scanning...",
                sizeof(g_state.net.wifi_scan_status) - 1);
        g_state.net.wifi_scan_status[sizeof(g_state.net.wifi_scan_status) - 1] = '\0';
        g_state.ui_needs_update = true;
        data_unlock(g_state);
        wifi_set_state(WIFI_STATE_SCAN_RUNNING);
        Serial.println("[SCAN] Async scan started → SCAN_RUNNING");
    } else {
        // ── Start failed immediately ──
        data_lock(g_state);
        g_state.net.wifi_scan_start_pending  = false;
        g_state.net.wifi_scan_radio_warming  = false;
        g_state.net.wifi_scan_active         = false;
        g_state.net.wifi_scan_error          = true;
        g_state.net.wifi_scan_done           = false;
        g_state.net.wifi_scan_start_attempts++;
        snprintf(g_state.net.wifi_scan_status, sizeof(g_state.net.wifi_scan_status),
                 "Scan start failed (%d)", result);
        g_state.ui_needs_update = true;
        data_unlock(g_state);
        Serial.printf("[SCAN] Start failed immediately: %d → SCAN_DONE\n", result);
        wifi_set_state(WIFI_STATE_SCAN_DONE);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
static void handle_scan_running() {
    // Timeout guard
    uint32_t elapsed = millis() - wifi_state_entered_ms;
    if (elapsed > WIFI_SCAN_TIMEOUT_MS) {
        WiFi.scanDelete();
        data_lock(g_state);
        g_state.net.wifi_scan_active      = false;
        g_state.net.wifi_scan_error       = true;
        g_state.net.wifi_scan_done        = false;
        g_state.net.wifi_scan_count       = 0;
        g_state.net.wifi_scan_has_results = false;
        strncpy(g_state.net.wifi_scan_status, "Scan timeout",
                sizeof(g_state.net.wifi_scan_status) - 1);
        g_state.net.wifi_scan_status[sizeof(g_state.net.wifi_scan_status) - 1] = '\0';
        g_state.ui_needs_update = true;
        data_unlock(g_state);
        Serial.println("[SCAN] Timeout → SCAN_DONE");
        wifi_set_state(WIFI_STATE_SCAN_DONE);
        return;
    }

    int result = WiFi.scanComplete();
    if (result == WIFI_SCAN_RUNNING) return;  // still in progress

    // ── Scan has a result (success or error) ──
    WiFiScanResult sorted[WIFI_SCAN_MAX_RESULTS] = {};
    uint8_t stored = 0;

    if (result > 0) {
        stored = (result > (int)WIFI_SCAN_MAX_RESULTS) ? WIFI_SCAN_MAX_RESULTS : (uint8_t)result;
        for (uint8_t i = 0; i < stored; i++) {
            String ssid = WiFi.SSID(i);
            strncpy(sorted[i].ssid, ssid.c_str(), sizeof(sorted[i].ssid) - 1);
            sorted[i].ssid[sizeof(sorted[i].ssid) - 1] = '\0';
            sorted[i].rssi       = WiFi.RSSI(i);
            sorted[i].encryption = (uint8_t)WiFi.encryptionType(i);
            sorted[i].channel    = (uint8_t)WiFi.channel(i);
        }
        // Sort by RSSI (descending = strongest first)
        for (uint8_t i = 0; i < stored; i++) {
            for (uint8_t j = i + 1; j < stored; j++) {
                if (sorted[j].rssi > sorted[i].rssi) {
                    WiFiScanResult tmp = sorted[i];
                    sorted[i] = sorted[j];
                    sorted[j] = tmp;
                }
            }
        }
    }

    WiFi.scanDelete();

    data_lock(g_state);
    g_state.net.wifi_scan_active       = false;
    g_state.net.wifi_scan_done         = (result >= 0);
    g_state.net.wifi_scan_error        = (result < 0);
    g_state.net.wifi_scan_count        = stored;
    g_state.net.wifi_scan_has_results  = (stored > 0);
    g_state.net.wifi_scan_finished_ts  = millis();
    memset(g_state.net.wifi_scan_results, 0, sizeof(g_state.net.wifi_scan_results));
    if (stored > 0) {
        memcpy(g_state.net.wifi_scan_results, sorted, sizeof(WiFiScanResult) * stored);
    }
    if (result < 0) {
        snprintf(g_state.net.wifi_scan_status, sizeof(g_state.net.wifi_scan_status),
                 "Scan failed (%d)", result);
    } else if (stored == 0) {
        strncpy(g_state.net.wifi_scan_status, "No networks found",
                sizeof(g_state.net.wifi_scan_status) - 1);
    } else {
        snprintf(g_state.net.wifi_scan_status, sizeof(g_state.net.wifi_scan_status),
                 "Found %u network(s)", stored);
    }
    g_state.net.wifi_scan_status[sizeof(g_state.net.wifi_scan_status) - 1] = '\0';
    g_state.ui_needs_update = true;
    data_unlock(g_state);

    Serial.printf("[SCAN] Done: raw=%d stored=%u → SCAN_DONE\n", result, stored);
    wifi_set_state(WIFI_STATE_SCAN_DONE);
}

// ─────────────────────────────────────────────────────────────────────────────
static void handle_scan_done() {
    // Decide what to do now that the scan is finished.
    if (wifi_restore_after_scan && wifi_power_policy_on) {
        wifi_restore_after_scan = false;
        Serial.println("[SCAN] Restoring saved WiFi connection after scan");
        wifi_do_begin();   // → CONNECTING (or IDLE if no credentials)
    } else {
        wifi_restore_after_scan = false;
        if (!wifi_power_policy_on) {
            WiFi.mode(WIFI_OFF);
            Serial.println("[WIFI] Power OFF after scan (policy)");
        }
        wifi_push_status("Scan complete", false);
        wifi_set_state(WIFI_STATE_IDLE);
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────

void wifi_manager_init() {
    prefs.begin("wifi_cfg", false);
    WiFi.persistent(false);
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(false);   // State machine owns reconnect logic

    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    data_lock(g_state);
    strncpy(g_state.net.saved_wifi_ssid, ssid.c_str(), sizeof(g_state.net.saved_wifi_ssid) - 1);
    g_state.net.saved_wifi_ssid[sizeof(g_state.net.saved_wifi_ssid) - 1] = '\0';
    strncpy(g_state.net.saved_wifi_pass, pass.c_str(), sizeof(g_state.net.saved_wifi_pass) - 1);
    g_state.net.saved_wifi_pass[sizeof(g_state.net.saved_wifi_pass) - 1] = '\0';
    g_state.ui_needs_update = true;
    data_unlock(g_state);
    Serial.println("[WIFI] Manager initialized");
}

void wifi_manager_load_and_connect() {
    // Called once at boot to auto-connect using stored credentials.
    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    if (ssid.length() == 0) {
        Serial.println("[WIFI] No saved credentials — staying IDLE");
        wifi_push_status("No saved WiFi credentials", false);
        wifi_set_state(WIFI_STATE_IDLE);
        return;
    }
    Serial.printf("[WIFI] Auto-connecting to saved SSID: %s\n", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.begin(ssid.c_str(), pass.c_str());
    wifi_push_status("CONNECTING...", false);
    wifi_set_state(WIFI_STATE_CONNECTING);
}

void wifi_manager_connect(const char* ssid, const char* pass) {
    // Manual connect — save credentials and start connecting.
    // Aborts any in-progress scan cleanly.
    if (!ssid || strlen(ssid) == 0) return;

    if (wifi_state == WIFI_STATE_SCAN_PREPARE ||
        wifi_state == WIFI_STATE_SCAN_RUNNING) {
        WiFi.scanDelete();
        wifi_clear_scan_flags();
        Serial.println("[WIFI] Scan aborted for manual connect");
    }

    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass ? pass : "");

    data_lock(g_state);
    strncpy(g_state.net.saved_wifi_ssid, ssid, sizeof(g_state.net.saved_wifi_ssid) - 1);
    g_state.net.saved_wifi_ssid[sizeof(g_state.net.saved_wifi_ssid) - 1] = '\0';
    strncpy(g_state.net.saved_wifi_pass, pass ? pass : "",
            sizeof(g_state.net.saved_wifi_pass) - 1);
    g_state.net.saved_wifi_pass[sizeof(g_state.net.saved_wifi_pass) - 1] = '\0';
    data_unlock(g_state);

    Serial.printf("[WIFI] Manual connect → SSID: %s\n", ssid);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    WiFi.begin(ssid, pass);
    wifi_push_status("CONNECTING...", false);
    wifi_restore_after_scan = false;
    wifi_set_state(WIFI_STATE_CONNECTING);
}

void wifi_manager_reconnect() {
    // User-triggered reconnect (e.g. tap Reconnect button from FAILED state).
    if (wifi_state == WIFI_STATE_SCAN_PREPARE ||
        wifi_state == WIFI_STATE_SCAN_RUNNING) {
        Serial.println("[WIFI] Reconnect ignored: scan in progress");
        return;
    }
    Serial.println("[WIFI] Manual reconnect requested");
    wifi_do_begin();
}

void wifi_manager_set_power(bool on) {
    wifi_power_policy_on = on;

    // Defer power-off if a scan is running — it will off after scan completes.
    if (!on && (wifi_state == WIFI_STATE_SCAN_PREPARE ||
                wifi_state == WIFI_STATE_SCAN_RUNNING)) {
        Serial.println("[WIFI] Power OFF deferred until scan completes");
        return;
    }

    if (on) {
        Serial.println("[WIFI] Power policy → ON");
        if (wifi_state == WIFI_STATE_IDLE) {
            wifi_manager_load_and_connect();
        }
    } else {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        data_lock(g_state);
        strcpy(g_state.net.connected_wifi_ssid, "-");
        strncpy(g_state.net.wifi_status_detail, "WiFi off",
                sizeof(g_state.net.wifi_status_detail) - 1);
        g_state.net.wifi_status_detail[sizeof(g_state.net.wifi_status_detail) - 1] = '\0';
        g_state.net.wifi_connected = false;
        g_state.ui_needs_update    = true;
        data_unlock(g_state);
        wifi_set_state(WIFI_STATE_IDLE);
        Serial.println("[WIFI] Power OFF");
    }
}

void wifi_manager_scan_request() {
    // Guard: ignore if scan already active in any phase.
    if (wifi_state == WIFI_STATE_SCAN_PREPARE ||
        wifi_state == WIFI_STATE_SCAN_RUNNING  ||
        wifi_state == WIFI_STATE_SCAN_DONE) {
        data_lock(g_state);
        strncpy(g_state.net.wifi_scan_status, "Scan already in progress",
                sizeof(g_state.net.wifi_scan_status) - 1);
        g_state.net.wifi_scan_status[sizeof(g_state.net.wifi_scan_status) - 1] = '\0';
        g_state.ui_needs_update = true;
        data_unlock(g_state);
        Serial.println("[SCAN] Request ignored: already scanning");
        return;
    }

    // Arm the scan request flag — wifi_manager_loop() will pick it up on the
    // next tick and transition to SCAN_PREPARE from whatever state we are in.
    data_lock(g_state);
    g_state.net.wifi_scan_requested    = true;
    g_state.net.wifi_scan_done         = false;
    g_state.net.wifi_scan_error        = false;
    g_state.net.wifi_scan_has_results  = false;
    g_state.net.wifi_scan_start_attempts = 0;
    strncpy(g_state.net.wifi_scan_status, "Scan requested...",
            sizeof(g_state.net.wifi_scan_status) - 1);
    g_state.net.wifi_scan_status[sizeof(g_state.net.wifi_scan_status) - 1] = '\0';
    g_state.ui_needs_update = true;
    data_unlock(g_state);
    Serial.println("[SCAN] Request queued");
}

WifiState wifi_manager_get_state() {
    return wifi_state;
}

// ─── Main loop (called from Task_Net every ~30 ms) ────────────────────────────
void wifi_manager_loop() {
    switch (wifi_state) {
        case WIFI_STATE_IDLE:          handle_idle();          break;
        case WIFI_STATE_CONNECTING:    handle_connecting();    break;
        case WIFI_STATE_CONNECTED:     handle_connected();     break;
        case WIFI_STATE_FAILED:        handle_failed();        break;
        case WIFI_STATE_SCAN_PREPARE:  handle_scan_prepare();  break;
        case WIFI_STATE_SCAN_RUNNING:  handle_scan_running();  break;
        case WIFI_STATE_SCAN_DONE:     handle_scan_done();     break;
    }
}
