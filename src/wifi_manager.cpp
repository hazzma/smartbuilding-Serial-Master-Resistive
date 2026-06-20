#include "wifi_manager.h"
#include "data.h"

static Preferences prefs;
static bool wifi_power_policy_on = true;
static bool scan_started_by_manager = false;
static bool wifi_scan_restore_connect = false;
static bool wifi_connecting = false;
static bool wifi_was_connected = false;
static int wifi_reconnect_attempts = 0;
static uint32_t wifi_connect_start_ms = 0;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
static const uint32_t WIFI_SCAN_TIMEOUT_MS = 15000;
static const uint32_t WIFI_SCAN_RADIO_WARMUP_MS = 900;
static const uint32_t WIFI_SCAN_RETRY_DELAY_MS = 900;
static const uint8_t WIFI_SCAN_START_MAX_ATTEMPTS = 2;
static uint32_t wifi_scan_start_ready_ms = 0;
static uint32_t wifi_scan_retry_at_ms = 0;

void wifi_manager_init() {
    prefs.begin("wifi_cfg", false);
    WiFi.setSleep(false);

    String ssid = prefs.getString("ssid", "");
    String pass = prefs.getString("pass", "");
    data_lock(g_state);
    strncpy(g_state.net.saved_wifi_ssid, ssid.c_str(), sizeof(g_state.net.saved_wifi_ssid) - 1);
    g_state.net.saved_wifi_ssid[sizeof(g_state.net.saved_wifi_ssid) - 1] = '\0';
    strncpy(g_state.net.saved_wifi_pass, pass.c_str(), sizeof(g_state.net.saved_wifi_pass) - 1);
    g_state.net.saved_wifi_pass[sizeof(g_state.net.saved_wifi_pass) - 1] = '\0';
    g_state.ui_needs_update = true;
    data_unlock(g_state);
}

void wifi_manager_set_power(bool on) {
    wifi_power_policy_on = on;
    if (!on && scan_started_by_manager) {
        Serial.println("[WIFI] Power OFF deferred until scan completes");
        return;
    }

    if (on) {
        WiFi.mode(WIFI_STA);
        Serial.println("[WIFI] Power ON");
    } else {
        WiFi.mode(WIFI_OFF);
        Serial.println("[WIFI] Power OFF");
        data_lock(g_state);
        strcpy(g_state.net.connected_wifi_ssid, "-");
        strcpy(g_state.net.wifi_status_detail, "IDLE: WiFi off");
        g_state.net.wifi_connected = false;
        g_state.ui_needs_update = true;
        data_unlock(g_state);
    }
}

void wifi_manager_connect(const char* ssid, const char* pass) {
    if (!ssid || strlen(ssid) == 0) return;
    Serial.printf("[WIFI] Connecting to: %s\n", ssid);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.disconnect();
    WiFi.begin(ssid, pass);
    wifi_connecting = true;
    wifi_reconnect_attempts = 0;
    wifi_connect_start_ms = millis();
    wifi_was_connected = false;
    data_lock(g_state);
    strncpy(g_state.net.saved_wifi_ssid, ssid, sizeof(g_state.net.saved_wifi_ssid) - 1);
    g_state.net.saved_wifi_ssid[sizeof(g_state.net.saved_wifi_ssid) - 1] = '\0';
    strncpy(g_state.net.saved_wifi_pass, pass ? pass : "", sizeof(g_state.net.saved_wifi_pass) - 1);
    g_state.net.saved_wifi_pass[sizeof(g_state.net.saved_wifi_pass) - 1] = '\0';
    strcpy(g_state.net.wifi_status_detail, "CONNECTING (1/3)...");
    g_state.ui_needs_update = true;
    data_unlock(g_state);
}

void wifi_manager_load_and_connect() {
    String ssid = prefs.getString("ssid", "han");
    String pass = prefs.getString("pass", "hanhanhan");
    if (ssid.length() > 0) {
        Serial.printf("[WIFI] Auto-connecting to SSID: %s\n", ssid.c_str());
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(false);
        WiFi.begin(ssid.c_str(), pass.c_str());
        wifi_connecting = true;
        wifi_reconnect_attempts = 0;
        wifi_connect_start_ms = millis();
        wifi_was_connected = false;
    } else {
        Serial.println("[WIFI] No saved credentials found.");
    }
}

void wifi_manager_reconnect() {
    String ssid = prefs.getString("ssid", "han");
    String pass = prefs.getString("pass", "hanhanhan");
    if (ssid.length() > 0) {
        Serial.printf("[WIFI] Manual Reconnect to: %s\n", ssid.c_str());
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(false);
        WiFi.disconnect();
        WiFi.begin(ssid.c_str(), pass.c_str());
        wifi_connecting = true;
        wifi_reconnect_attempts = 0;
        wifi_connect_start_ms = millis();
        wifi_was_connected = false;
        data_lock(g_state);
        strcpy(g_state.net.wifi_status_detail, "CONNECTING (1/3)...");
        g_state.ui_needs_update = true;
        data_unlock(g_state);
    }
}

void wifi_manager_scan_request() {
    // Cegah scan jika WiFi sedang dalam proses koneksi
    if (wifi_connecting) {
        Serial.println("[SCAN] Ignored: WiFi is connecting");
        data_lock(g_state);
        strncpy(g_state.net.wifi_scan_status, "Busy connecting...", sizeof(g_state.net.wifi_scan_status) - 1);
        g_state.net.wifi_scan_status[sizeof(g_state.net.wifi_scan_status) - 1] = '\0';
        g_state.ui_needs_update = true;
        data_unlock(g_state);
        return;
    }

    data_lock(g_state);
    if (g_state.net.wifi_scan_active ||
        g_state.net.wifi_scan_start_pending ||
        g_state.net.wifi_scan_radio_warming) {
        strncpy(g_state.net.wifi_scan_status, "Scan already running...", sizeof(g_state.net.wifi_scan_status) - 1);
        g_state.net.wifi_scan_status[sizeof(g_state.net.wifi_scan_status) - 1] = '\0';
        g_state.ui_needs_update = true;
        data_unlock(g_state);
        Serial.println("[SCAN] Request ignored: scan already running");
        return;
    }

    g_state.net.wifi_scan_requested = true;
    g_state.net.wifi_scan_active = false;
    g_state.net.wifi_scan_start_pending = false;
    g_state.net.wifi_scan_radio_warming = false;
    g_state.net.wifi_scan_done = false;
    g_state.net.wifi_scan_error = false;
    g_state.net.wifi_scan_start_attempts = 0;
    g_state.net.wifi_scan_requested_ts = millis();
    strncpy(g_state.net.wifi_scan_status, "Scan requested...", sizeof(g_state.net.wifi_scan_status) - 1);
    g_state.net.wifi_scan_status[sizeof(g_state.net.wifi_scan_status) - 1] = '\0';
    g_state.ui_needs_update = true;
    data_unlock(g_state);
    Serial.println("[SCAN] Request queued");
}

static void wifi_scan_sort_results(WiFiScanResult* results, uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        for (uint8_t j = i + 1; j < count; j++) {
            if (results[j].rssi > results[i].rssi) {
                WiFiScanResult temp = results[i];
                results[i] = results[j];
                results[j] = temp;
            }
        }
    }
}

static void wifi_scan_restore_after_stop() {
    scan_started_by_manager = false;

    if (!wifi_power_policy_on) {
        WiFi.mode(WIFI_OFF);
        wifi_scan_restore_connect = false;
        Serial.println("[WIFI] Power OFF after scan");
        return;
    }

    WiFi.setAutoReconnect(false);

    if (!wifi_scan_restore_connect) return;

    wifi_scan_restore_connect = false;
    String ssid = prefs.getString("ssid", "han");
    String pass = prefs.getString("pass", "hanhanhan");
    if (ssid.length() == 0) return;

    Serial.printf("[WIFI] Restoring connection to: %s\n", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());
    wifi_connecting = true;
    wifi_reconnect_attempts = 0;
    wifi_connect_start_ms = millis();
    wifi_was_connected = false;

    data_lock(g_state);
    strcpy(g_state.net.wifi_status_detail, "CONNECTING (1/3)...");
    g_state.ui_needs_update = true;
    data_unlock(g_state);
}

static void wifi_scan_prepare_start() {
    if (WiFi.getMode() != WIFI_STA) {
        WiFi.mode(WIFI_STA);
    }

    // Save whether we need to restore connection later
    wifi_scan_restore_connect = wifi_power_policy_on && (prefs.getString("ssid", "").length() > 0);

    Serial.println("[SCAN] Disconnecting and disabling auto-reconnect for scan");
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);

    WiFi.scanDelete();
    wifi_scan_start_ready_ms = millis() + WIFI_SCAN_RADIO_WARMUP_MS;
    wifi_scan_retry_at_ms = 0;

    data_lock(g_state);
    g_state.net.wifi_scan_requested = false;
    g_state.net.wifi_scan_start_pending = true;
    g_state.net.wifi_scan_radio_warming = true;
    g_state.net.wifi_scan_start_attempts = 0;
    strncpy(g_state.net.wifi_scan_status,
            wifi_scan_restore_connect ? "Pausing WiFi for scan..." : "Preparing WiFi radio...",
            sizeof(g_state.net.wifi_scan_status) - 1);
    g_state.net.wifi_scan_status[sizeof(g_state.net.wifi_scan_status) - 1] = '\0';
    g_state.ui_needs_update = true;
    data_unlock(g_state);

    Serial.println("[SCAN] Preparing WiFi radio");
}

static void wifi_scan_fail(int code, const char* message) {
    WiFi.scanDelete();

    data_lock(g_state);
    g_state.net.wifi_scan_requested = false;
    g_state.net.wifi_scan_start_pending = false;
    g_state.net.wifi_scan_radio_warming = false;
    g_state.net.wifi_scan_active = false;
    g_state.net.wifi_scan_error = true;
    g_state.net.wifi_scan_done = false;
    g_state.net.wifi_scan_count = 0;
    g_state.net.wifi_scan_has_results = false;
    memset(g_state.net.wifi_scan_results, 0, sizeof(g_state.net.wifi_scan_results));
    snprintf(g_state.net.wifi_scan_status, sizeof(g_state.net.wifi_scan_status), "%s (%d)", message, code);
    g_state.ui_needs_update = true;
    data_unlock(g_state);

    wifi_scan_restore_after_stop();
}

static void wifi_scan_start_async() {
    uint8_t attempts = 0;
    data_lock(g_state);
    attempts = g_state.net.wifi_scan_start_attempts;
    data_unlock(g_state);

    if (millis() < wifi_scan_start_ready_ms || millis() < wifi_scan_retry_at_ms) return;

    if (WiFi.getMode() == WIFI_OFF) {
        WiFi.mode(WIFI_STA);
        wifi_scan_start_ready_ms = millis() + WIFI_SCAN_RADIO_WARMUP_MS;
        return;
    }

    WiFi.scanDelete();
    int started = WiFi.scanNetworks(true, true);
    uint32_t now = millis();

    data_lock(g_state);
    g_state.net.wifi_scan_start_attempts = attempts + 1;
    g_state.net.wifi_scan_started_ts = now;
    g_state.net.wifi_scan_done = false;
    g_state.net.wifi_scan_error = false;

    if (started == WIFI_SCAN_RUNNING) {
        g_state.net.wifi_scan_start_pending = false;
        g_state.net.wifi_scan_radio_warming = false;
        g_state.net.wifi_scan_active = true;
        strncpy(g_state.net.wifi_scan_status, "Scanning...", sizeof(g_state.net.wifi_scan_status) - 1);
        Serial.println("[SCAN] Async scan started");
    } else {
        g_state.net.wifi_scan_active = false;
        g_state.net.wifi_scan_radio_warming = false;
        snprintf(g_state.net.wifi_scan_status, sizeof(g_state.net.wifi_scan_status), "Scan busy, retrying...");
        Serial.printf("[SCAN] Start attempt %u failed: %d\n", attempts + 1, started);
    }

    g_state.net.wifi_scan_status[sizeof(g_state.net.wifi_scan_status) - 1] = '\0';
    g_state.ui_needs_update = true;
    data_unlock(g_state);

    if (started == WIFI_SCAN_RUNNING) {
        scan_started_by_manager = true;
        return;
    }

    if (attempts + 1 >= WIFI_SCAN_START_MAX_ATTEMPTS) {
        wifi_scan_fail(started, "Scan start failed");
    } else {
        wifi_scan_retry_at_ms = millis() + WIFI_SCAN_RETRY_DELAY_MS;
    }
}

static void wifi_scan_finish(int count, bool timed_out) {
    WiFiScanResult results[WIFI_SCAN_MAX_RESULTS] = {};
    uint8_t stored = 0;

    if (!timed_out && count > 0) {
        stored = (count > WIFI_SCAN_MAX_RESULTS) ? WIFI_SCAN_MAX_RESULTS : count;
        for (uint8_t i = 0; i < stored; i++) {
            String ssid = WiFi.SSID(i);
            strncpy(results[i].ssid, ssid.c_str(), sizeof(results[i].ssid) - 1);
            results[i].ssid[sizeof(results[i].ssid) - 1] = '\0';
            results[i].rssi = WiFi.RSSI(i);
            results[i].encryption = (uint8_t)WiFi.encryptionType(i);
            results[i].channel = (uint8_t)WiFi.channel(i);
        }
        wifi_scan_sort_results(results, stored);
    }

    WiFi.scanDelete();

    data_lock(g_state);
    g_state.net.wifi_scan_active = false;
    g_state.net.wifi_scan_start_pending = false;
    g_state.net.wifi_scan_radio_warming = false;
    g_state.net.wifi_scan_done = !timed_out && count >= 0;
    g_state.net.wifi_scan_error = timed_out || count < 0;
    g_state.net.wifi_scan_count = stored;
    g_state.net.wifi_scan_has_results = stored > 0;
    g_state.net.wifi_scan_finished_ts = millis();
    memset(g_state.net.wifi_scan_results, 0, sizeof(g_state.net.wifi_scan_results));
    if (stored > 0) {
        memcpy(g_state.net.wifi_scan_results, results, sizeof(WiFiScanResult) * stored);
    }

    if (timed_out) {
        strncpy(g_state.net.wifi_scan_status, "Scan timeout", sizeof(g_state.net.wifi_scan_status) - 1);
    } else if (count < 0) {
        strncpy(g_state.net.wifi_scan_status, "Scan failed", sizeof(g_state.net.wifi_scan_status) - 1);
    } else if (stored == 0) {
        strncpy(g_state.net.wifi_scan_status, "No networks found", sizeof(g_state.net.wifi_scan_status) - 1);
    } else {
        snprintf(g_state.net.wifi_scan_status, sizeof(g_state.net.wifi_scan_status), "Found %u network(s)", stored);
    }

    g_state.net.wifi_scan_status[sizeof(g_state.net.wifi_scan_status) - 1] = '\0';
    g_state.ui_needs_update = true;
    data_unlock(g_state);

    Serial.printf("[SCAN] Complete: raw=%d stored=%u timeout=%s\n",
                  count, stored, timed_out ? "yes" : "no");

    wifi_scan_restore_after_stop();
}

static void wifi_scan_loop() {
    bool should_start = false;
    bool start_pending = false;
    bool is_active = false;
    uint32_t started_ts = 0;

    data_lock(g_state);
    should_start = g_state.net.wifi_scan_requested && !g_state.net.wifi_scan_active;
    start_pending = g_state.net.wifi_scan_start_pending;
    is_active = g_state.net.wifi_scan_active;
    started_ts = g_state.net.wifi_scan_started_ts;
    data_unlock(g_state);

    if (should_start) {
        wifi_scan_prepare_start();
        return;
    }

    if (start_pending) {
        wifi_scan_start_async();
        return;
    }

    if (!is_active) return;

    int scan_status = WiFi.scanComplete();
    if (scan_status == WIFI_SCAN_RUNNING) {
        if (millis() - started_ts > WIFI_SCAN_TIMEOUT_MS) {
            wifi_scan_finish(scan_status, true);
        }
        return;
    }

    if (scan_status < 0) {
        wifi_scan_fail(scan_status, "Scan failed");
        return;
    }

    wifi_scan_finish(scan_status, false);
}

void wifi_manager_loop() {
    wifi_scan_loop();

    // Check if scan is running or pending to avoid collision
    data_lock(g_state);
    bool scan_in_progress = g_state.net.wifi_scan_active ||
                            g_state.net.wifi_scan_start_pending ||
                            g_state.net.wifi_scan_radio_warming;
    data_unlock(g_state);

    if (scan_in_progress) {
        return;
    }

    static uint32_t last_check = 0;
    if (millis() - last_check > 1000) {
        last_check = millis();
        wl_status_t status = WiFi.status();
        char next_ssid[32] = "-";
        char next_detail[64] = "DISCONNECTED";

        if (wifi_connecting) {
            if (status == WL_CONNECTED) {
                wifi_connecting = false;
                wifi_was_connected = true;
                wifi_reconnect_attempts = 0;
                strncpy(next_ssid, WiFi.SSID().c_str(), sizeof(next_ssid) - 1);
                next_ssid[sizeof(next_ssid) - 1] = '\0';
                strcpy(next_detail, "SUCCESS: Connected");
                Serial.println("[WIFI] Connection successful");
            } else {
                bool attempt_failed = (millis() - wifi_connect_start_ms > WIFI_CONNECT_TIMEOUT_MS) ||
                                      (status == WL_CONNECT_FAILED) ||
                                      (status == WL_NO_SSID_AVAIL);

                if (attempt_failed) {
                    wifi_reconnect_attempts++;
                    Serial.printf("[WIFI] Connection attempt failed. Retry count: %d/3\n", wifi_reconnect_attempts);

                    if (wifi_reconnect_attempts < 3) {
                        String ssid = prefs.getString("ssid", "");
                        String pass = prefs.getString("pass", "");
                        WiFi.disconnect(false, false);
                        WiFi.begin(ssid.c_str(), pass.c_str());
                        wifi_connect_start_ms = millis();
                        snprintf(next_detail, sizeof(next_detail), "RETRETING (%d/3)...", wifi_reconnect_attempts + 1);
                    } else {
                        wifi_connecting = false;
                        wifi_was_connected = false;
                        WiFi.disconnect(true, false);
                        strcpy(next_detail, "FAILED: Max retries reached");
                        Serial.println("[WIFI] Connection failed after 3 attempts. Stopping.");
                    }
                } else {
                    snprintf(next_detail, sizeof(next_detail), "CONNECTING (%d/3)...", wifi_reconnect_attempts + 1);
                }
            }
        } else {
            if (status == WL_CONNECTED) {
                wifi_was_connected = true;
                wifi_reconnect_attempts = 0;
                strncpy(next_ssid, WiFi.SSID().c_str(), sizeof(next_ssid) - 1);
                next_ssid[sizeof(next_ssid) - 1] = '\0';
                strcpy(next_detail, "SUCCESS: Connected");
            } else {
                if (wifi_was_connected && prefs.getString("ssid", "").length() > 0) {
                    Serial.println("[WIFI] Connection lost. Reconnecting (up to 3 retries)...");
                    String ssid = prefs.getString("ssid", "");
                    String pass = prefs.getString("pass", "");
                    WiFi.disconnect(false, false);
                    WiFi.begin(ssid.c_str(), pass.c_str());
                    wifi_connecting = true;
                    wifi_reconnect_attempts = 0;
                    wifi_connect_start_ms = millis();
                    wifi_was_connected = false;
                    strcpy(next_detail, "RECONNECTING (1/3)...");
                } else {
                    if (status == WL_IDLE_STATUS || status == WL_DISCONNECTED) {
                        strcpy(next_detail, "IDLE: Disconnected");
                    } else if (status == WL_CONNECT_FAILED) {
                        strcpy(next_detail, "FAILED: Wrong Credentials?");
                    } else if (status == WL_NO_SSID_AVAIL) {
                        strcpy(next_detail, "FAILED: SSID Not Found");
                    }
                }
            }
        }

        data_lock(g_state);
        bool changed = strcmp(g_state.net.connected_wifi_ssid, next_ssid) != 0 ||
                       strcmp(g_state.net.wifi_status_detail, next_detail) != 0 ||
                       g_state.net.wifi_connected != (status == WL_CONNECTED);

        g_state.net.wifi_connected = (status == WL_CONNECTED);
        strncpy(g_state.net.connected_wifi_ssid, next_ssid, sizeof(g_state.net.connected_wifi_ssid) - 1);
        g_state.net.connected_wifi_ssid[sizeof(g_state.net.connected_wifi_ssid) - 1] = '\0';
        strncpy(g_state.net.wifi_status_detail, next_detail, sizeof(g_state.net.wifi_status_detail) - 1);
        g_state.net.wifi_status_detail[sizeof(g_state.net.wifi_status_detail) - 1] = '\0';
        if (changed) g_state.ui_needs_update = true;
        data_unlock(g_state);
    }
}
