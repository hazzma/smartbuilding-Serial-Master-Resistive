#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>

// ─── WiFi State Enum ─────────────────────────────────────────────────────────
// Single source of truth for what the WiFi manager is currently doing.
// Only one state is active at a time — no process overlaps.
enum WifiState : uint8_t {
    WIFI_STATE_IDLE         = 0,  // WiFi OFF or power policy OFF, nothing active
    WIFI_STATE_CONNECTING,        // WiFi.begin() called, waiting for WL_CONNECTED
    WIFI_STATE_CONNECTED,         // WL_CONNECTED confirmed, MQTT may proceed
    WIFI_STATE_FAILED,            // Connect failed / timed out — waiting for user action
    WIFI_STATE_SCAN_PREPARE,      // Pausing STA (disconnect + settle) before scan
    WIFI_STATE_SCAN_RUNNING,      // Async scanNetworks() in progress
    WIFI_STATE_SCAN_DONE,         // Scan finished — transitioning to CONNECTING or IDLE
};

// ─── Public API ───────────────────────────────────────────────────────────────
void wifi_manager_init();
void wifi_manager_load_and_connect();
void wifi_manager_connect(const char* ssid, const char* pass);
void wifi_manager_reconnect();
void wifi_manager_set_power(bool on);
void wifi_manager_scan_request();
void wifi_manager_loop();

// Returns the current internal WiFi state for diagnostics / logging
WifiState wifi_manager_get_state();

#endif
