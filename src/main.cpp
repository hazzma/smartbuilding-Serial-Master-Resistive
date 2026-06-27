#include <Arduino.h>
#include "display.h"
#include "data.h"
#include "ui_screens.h"
#include "ui_widgets.h"
#include "touch.h"
#include <WiFi.h>
#include "mqtt_manager.h"
#include "lan_manager.h"
#include "wifi_manager.h"
#include "time_manager.h"
#include "rs485_manager.h"
#include "mapping_manager.h"

#define Serial if (g_serial_log_mode == LOG_DATA) Serial

// Set to 1 to inject a local dummy RS485 slave for UI/home-screen testing.
// Set to 0 to remove the dummy slave and use only real discovered devices.
#define ENABLE_DUMMY_RS485_SLAVE 0

static const uint64_t DUMMY_RS485_MAC = 0xD00D00000123ULL;
static const uint32_t AC_PERFORMANCE_WINDOW_MS          = 10UL * 60UL * 1000UL; // 10 min
static const float    AC_PERFORMANCE_MIN_START_GAP_C      = 2.0f;
static const float    AC_PERFORMANCE_MIN_DROP_C           = 1.0f;   // 1°C
static const float    AC_PERFORMANCE_ESCALATE_THRESHOLD_C = 0.1f;   // "no change" threshold
static const float    AC_PERFORMANCE_TARGET_RESET_DELTA_C = 0.5f;
static const uint32_t AUTO_OFF_DELAY_MS                   = 5UL * 60UL * 1000UL; // 5 min
static const uint32_t LED_CHECK_WINDOW_MS                 = 15UL * 1000UL; // 15 sec
static const float    LED_CHECK_MIN_DELTA_LUX             = 50.0f;
static const uint8_t  PRECLASS_FAN_SPEED                  = 1;  // Low

static bool dashboard_average_temp_locked(const BuildingState& state, float& average_c) {
    float sum = 0.0f;
    uint8_t count = 0;
    for (uint8_t i = 0; i < DASHBOARD_TEMP_SLOTS; i++) {
        if (!state.rs485.dashboard.temp_valid[i]) continue;
        sum += state.rs485.dashboard.temp[i];
        count++;
    }
    if (count == 0) return false;
    average_c = sum / count;
    return true;
}

// Returns: bit0 = warning_changed, bit1 = fan_escalation_triggered
static uint8_t update_ac_performance_monitor_locked(BuildingState& state, uint32_t now) {
    float room_temp_c = -100.0f;
    bool temp_valid = dashboard_average_temp_locked(state, room_temp_c);
    bool ac_available = state.rs485.dashboard.ac_available;
    bool should_monitor = state.sensor.ac_on && ac_available && temp_valid &&
                          room_temp_c >= state.sensor.temp_target + AC_PERFORMANCE_MIN_START_GAP_C;
    bool target_changed =
        fabsf(state.sensor.temp_target - state.sensor.ac_performance_target_c) >=
        AC_PERFORMANCE_TARGET_RESET_DELTA_C;
    bool warning_before = state.sensor.ac_performance_warning;

    // If AC is OFF — reset monitor and clear warning
    if (!state.sensor.ac_on) {
        state.sensor.ac_performance_monitor_active = false;
        state.sensor.ac_performance_started_ms = 0;
        state.sensor.ac_performance_start_temp_c = -100.0f;
        state.sensor.ac_performance_target_c = state.sensor.temp_target;
        state.sensor.ac_performance_warning = false;
        state.sensor.ac_fan_escalated = false;
        return (warning_before != state.sensor.ac_performance_warning) ? 1 : 0;
    }

    if (!should_monitor || target_changed) {
        // Conditions not met (AC too close to target, or target changed) — reset window but keep warning
        state.sensor.ac_performance_monitor_active = false;
        state.sensor.ac_performance_started_ms = 0;
        state.sensor.ac_performance_start_temp_c = -100.0f;
        state.sensor.ac_performance_target_c = state.sensor.temp_target;
        if (target_changed) {
            state.sensor.ac_performance_warning = false;
            state.sensor.ac_fan_escalated = false;
        }
        return (warning_before != state.sensor.ac_performance_warning) ? 1 : 0;
    }

    if (!state.sensor.ac_performance_monitor_active) {
        state.sensor.ac_performance_monitor_active = true;
        state.sensor.ac_performance_started_ms = now;
        state.sensor.ac_performance_start_temp_c = room_temp_c;
        state.sensor.ac_performance_target_c = state.sensor.temp_target;
        Serial.printf("[AC Monitor] Started room=%.1fC target=%.1fC window=10min\n",
                      room_temp_c, state.sensor.temp_target);
        return 0;
    }

    if (now - state.sensor.ac_performance_started_ms >= AC_PERFORMANCE_WINDOW_MS) {
        float drop_c = state.sensor.ac_performance_start_temp_c - room_temp_c;
        state.sensor.ac_performance_warning = (drop_c < AC_PERFORMANCE_MIN_DROP_C);
        Serial.printf("[AC Monitor] Window done start=%.1fC now=%.1fC drop=%.1fC warning=%s\n",
                      state.sensor.ac_performance_start_temp_c,
                      room_temp_c,
                      drop_c,
                      state.sensor.ac_performance_warning ? "YES" : "NO");

        uint8_t result = (warning_before != state.sensor.ac_performance_warning) ? 1 : 0;

        // Fan escalation: if no change at all (<0.1°C), escalate to max fan speed
        if (drop_c < AC_PERFORMANCE_ESCALATE_THRESHOLD_C && !state.sensor.ac_fan_escalated) {
            state.sensor.ac_fan_speed = 3;  // Max fan speed
            state.sensor.ac_fan_escalated = true;
            result |= 2;  // signal caller to send RS485 + publish
            Serial.println("[AC Monitor] No temp change at all — fan escalated to MAX");
        }

        // Restart rolling window (warning persists until AC off)
        state.sensor.ac_performance_started_ms = now;
        state.sensor.ac_performance_start_temp_c = room_temp_c;
        return result;
    }

    return 0;
}

static void apply_dummy_rs485_slave(bool enabled) {
    data_lock(g_state);

    bool changed = false;
    uint8_t count = g_state.rs485.slave_count;
    if (count > RS485_MAX_SLAVES) count = RS485_MAX_SLAVES;

    if (!enabled) {
        for (uint8_t i = 0; i < count; i++) {
            if (g_state.rs485.slaves[i].uid != RS485_DUMMY_UI_UID) continue;

            for (uint8_t j = i; j + 1 < count; j++) {
                g_state.rs485.slaves[j] = g_state.rs485.slaves[j + 1];
            }
            memset(&g_state.rs485.slaves[count - 1], 0, sizeof(g_state.rs485.slaves[count - 1]));
            g_state.rs485.slave_count = count > 1 ? count - 1 : 1;
            changed = true;
            break;
        }

        if (changed || g_state.rs485.slave_count == 0) {
            if (g_state.rs485.slave_count == 0) g_state.rs485.slave_count = 1;
            if (g_state.rs485.slaves[0].address == 0 && g_state.rs485.slaves[0].uid == 0) {
                strcpy(g_state.rs485.slaves[0].name, "Device 0");
            }
        }
    } else {
        uint8_t index = RS485_MAX_SLAVES;
        for (uint8_t i = 0; i < count; i++) {
            if (g_state.rs485.slaves[i].uid == RS485_DUMMY_UI_UID) {
                index = i;
                break;
            }
        }

        if (index >= RS485_MAX_SLAVES) {
            if (count == 1 && g_state.rs485.slaves[0].address == 0 && g_state.rs485.slaves[0].uid == 0) {
                index = 0;
            } else if (count < RS485_MAX_SLAVES) {
                index = count;
                g_state.rs485.slave_count = count + 1;
            }
        }

        if (index < RS485_MAX_SLAVES) {
            RS485SlaveState& slave = g_state.rs485.slaves[index];
            bool existing_dummy = slave.uid == RS485_DUMMY_UI_UID;
            uint16_t previous_enabled_mask = existing_dummy ? slave.enabled_mask : 0;
            uint8_t previous_temp_enabled_mask = existing_dummy ? slave.temp_enabled_mask : 0;
            memset(&slave, 0, sizeof(slave));
            slave.address = 0x10;
            slave.uid = RS485_DUMMY_UI_UID;
            slave.mac = DUMMY_RS485_MAC;
            strcpy(slave.name, "Dummy UI Node");
            slave.capability = CAP_TEMP | CAP_CO2 | CAP_HUMAN_PRESENCE |
                               CAP_AC_IR | CAP_PROJECTOR_IR | CAP_LIGHT_RELAY |
                               CAP_LUX | CAP_LCD_CTRL;
            slave.enabled_mask = existing_dummy ?
                                 (previous_enabled_mask & slave.capability) :
                                 slave.capability;
            slave.protocol_version = 1;
            slave.device_class = 1;
            slave.fw_version = 100;
            slave.temp_count = 4;
            slave.temp_enabled_mask = existing_dummy ? (previous_temp_enabled_mask & 0x0F) : 0x0F;
            slave.co2_count = 1;
            slave.presence_count = 1;
            slave.relay_count = 1;
            slave.ir_count = 2;
            slave.lux_count = 1;
            slave.lcd_count = 1;
            slave.temp[0] = 24.8f;
            slave.temp[1] = 25.1f;
            slave.temp[2] = 24.6f;
            slave.temp[3] = 25.4f;
            for (uint8_t i = 0; i < DASHBOARD_TEMP_SLOTS; i++) slave.temp_valid[i] = true;
            slave.co2 = 720;
            slave.co2_valid = true;
            slave.lux = 420.0f;
            slave.lux_valid = true;
            slave.human_presence = true;
            slave.human_presence_valid = true;
            slave.identity_synced = true;
            slave.capability_synced = true;
            slave.last_identity_ms = millis();
            slave.last_capability_ms = millis();
            slave.last_seen = millis();
            slave.online = true;
            slave.degraded = false;
            changed = true;
        }
    }

    if (changed) {
        mapping_manager_update_locked(g_state);
        g_state.ui_needs_update = true;
    }

    data_unlock(g_state);
}

// ─────────────────────────────────────────────────────────────────────────────
// Task_Net: Core 0 — Data & Logic Engine (Non-Visual)
// ─────────────────────────────────────────────────────────────────────────────
void Task_Net(void* pvParameters) {
    uint32_t last_sim_update = 0;
    uint32_t last_dummy_refresh = 0;

    wifi_manager_init();
    wifi_manager_load_and_connect();
    lan_manager_load_config();
    time_manager_init();
    mqtt_init();

    for (;;) {
        uint32_t now = millis();
        static int last_prio = -1;

        if (ENABLE_DUMMY_RS485_SLAVE == 1 && now - last_dummy_refresh > 1000) {
            last_dummy_refresh = now;
            apply_dummy_rs485_slave(true);
        }

        // Unified state update
        data_lock(g_state);
        int current_prio = g_state.net.net_priority;
        bool wifi_now_connected = (WiFi.status() == WL_CONNECTED);
        bool scan_in_progress = g_state.net.wifi_scan_active || g_state.net.wifi_scan_start_pending || g_state.net.wifi_scan_radio_warming;
        if (!scan_in_progress) {
            if (g_state.net.wifi_connected != wifi_now_connected) {
                g_state.net.wifi_connected = wifi_now_connected;
                g_state.ui_needs_update = true;
            }
        }
        bool lan_initialized = g_state.net.lan_initialized;

        // Data Timeout Check (10s) — FSD NET-003
        // NOTE: last_data_ts hanya di-update oleh RS485 polling, BUKAN oleh MQTT callback.
        // Jadi kalau slave mati, sensor data akan timeout dan direset ke invalid.
        if (now - g_state.last_data_ts > 10000 && !g_state.use_dummy) {
            bool changed = false;
            for (int i = 0; i < 4; i++) {
                if (g_state.sensor.temp[i] != -100.0f) {
                    g_state.sensor.temp[i] = -100.0f;
                    changed = true;
                }
            }
            if (g_state.sensor.lux >= 0.0f) {
                g_state.sensor.lux = -1.0f;
                changed = true;
            }
            if (g_state.sensor.co2 >= 0) {
                g_state.sensor.co2 = -1;
                changed = true;
            }
            // Also invalidate dashboard temp_valid from mapping_manager
            // (dashboard di-recompute oleh mapping_manager tiap RS485 polling,
            //  jadi kalau polling berhenti, dashboard temp_valid akan tetap,
            //  tapi kita set sensor.sensor_error untuk UI)
            for (int i = 0; i < 4; i++) {
                if (!g_state.sensor.sensor_error[i]) {
                    g_state.sensor.sensor_error[i] = true;
                    changed = true;
                }
            }
            if (changed) {
                g_state.ui_needs_update = true;
                Serial.println("[Timeout] RS485 data timeout - all sensors set invalid");
            }
            g_state.last_data_ts = now; // prevent re-trigger every loop
        }

        // Connection status string update
        if (g_state.net.mqtt_ok) {
            strncpy(g_state.net.conn_status, "CONNECTED", 31);
        } else if (g_state.net.wifi_connected || g_state.net.lan_connected) {
            strncpy(g_state.net.conn_status, "Connecting MQTT", 31);
        } else {
            strncpy(g_state.net.conn_status, "DISCONNECTED", 31);
        }

        // Simulation (if use_dummy)
        if (g_state.use_dummy && (now - last_sim_update > 500)) {
            last_sim_update = now;
            g_state.sensor.temp[0] += 0.01f;
            if (g_state.sensor.temp[0] > 35.0f) g_state.sensor.temp[0] = 20.0f;
            g_state.ui_needs_update = true;
        }
        data_unlock(g_state);

        if (current_prio != last_prio) {
            last_prio = current_prio;
            wifi_manager_set_power(current_prio == 0);
        }

        if (current_prio == 1 && !lan_initialized) {
            lan_init();
        }

        wifi_manager_loop();
        if (current_prio == 1) {
            lan_loop();
        }
        mqtt_loop();
        time_manager_update();

        // 1-second logic check for active-load accumulation, midnight rollover, and scheduler countdowns
        static uint32_t last_sec_check = 0;
        if (now - last_sec_check >= 1000) {
            last_sec_check = now;

            bool save_needed = false;
            bool trigger_rs485_ac_off = false;
            bool trigger_rs485_light_off = false;
            bool trigger_schedule_ac_on = false;
            bool trigger_schedule_light_on = false;
            bool trigger_schedule_publish = false;
            bool ac_performance_warning_changed = false;
            bool trigger_auto_off_ac = false;
            bool trigger_auto_off_light = false;
            bool trigger_auto_off_projector = false;
            bool trigger_fan_escalation = false;
            bool trigger_led_warning_changed = false;
            float target_temp = 23.0f;
            uint8_t fan_speed = 0;
            uint8_t swing_mode = 0;

            data_lock(g_state);

            bool class_schedule_active = false; // declared here so legacy/auto-off can use it

            // Accumulate daily active durations.
            if (g_state.sensor.light_on) {
                g_state.sensor.light_accum_sec_today++;
            }
            if (g_state.sensor.light_on || g_state.sensor.ac_on) {
                g_state.sensor.active_load_accum_sec_today++;
            }

            uint8_t ac_monitor_result = update_ac_performance_monitor_locked(g_state, now);
            ac_performance_warning_changed = (ac_monitor_result & 1) != 0;
            trigger_fan_escalation = (ac_monitor_result & 2) != 0;
            if (ac_performance_warning_changed || trigger_fan_escalation) {
                g_state.ui_needs_update = true;
            }

            // ─── NEW SCHEDULER: Weekly Session-Based ───
            // Session times (BINUS):
            // S1: 07:20-09:00  S2: 09:20-11:00  S3: 11:20-13:00
            // S4: 13:20-15:00  S5: 15:20-17:00  S6: 17:20-19:00
            struct tm timeinfo;
            if (getLocalTime(&timeinfo, 5)) {
                uint32_t today = (uint32_t)(timeinfo.tm_year + 1900) * 10000UL +
                                 (uint32_t)(timeinfo.tm_mon + 1) * 100UL +
                                 (uint32_t)timeinfo.tm_mday;
                uint16_t minute_now = (uint16_t)timeinfo.tm_hour * 60U + (uint16_t)timeinfo.tm_min;

                static int last_day = -1;
                if (last_day == -1) {
                    last_day = timeinfo.tm_mday;
                    
                    // Re-cache today's sessions on startup/sync
                    uint8_t today_idx = schedule_get_day_of_week();
                    if (today_idx < SCHEDULE_DAYS && g_state.sensor.sched_weekly.valid) {
                        g_state.sensor.sched_today_sessions_bitmask = g_state.sensor.sched_weekly.day_mask[today_idx];
                        g_state.sensor.sched_active_session_count = 0;
                        memset(g_state.sensor.sched_active_sessions, 0, sizeof(g_state.sensor.sched_active_sessions));
                        for (uint8_t s = 0; s < SCHEDULE_SESSION_COUNT; s++) {
                            bool active = (g_state.sensor.sched_today_sessions_bitmask & (1 << s)) != 0;
                            g_state.sensor.sched_active_sessions[s] = active;
                            if (active) g_state.sensor.sched_active_session_count++;
                        }
                        g_state.sensor.sched_last_triggered_min = 0;
                        g_state.sensor.sched_retry_pending = false;
                        g_state.sensor.sched_retry_check_ms = 0;
                        g_state.sensor.sched_retry_session = 0;
                        g_state.sensor.sched_pre_start_triggered_mask = 0;
                        g_state.sensor.sched_start_triggered_mask = 0;
                        Serial.printf("[Schedule] Startup refresh: today=%s bitmask=%02X sessions=%u\n",
                                      schedule_get_day_name(today_idx),
                                      g_state.sensor.sched_today_sessions_bitmask,
                                      g_state.sensor.sched_active_session_count);
                    }
                } else if (timeinfo.tm_mday != last_day) {
                    last_day = timeinfo.tm_mday;

                    // Rollover light history
                    uint16_t light_min = g_state.sensor.active_load_accum_sec_today / 60;
                    uint32_t current_day = g_state.sensor.light_day_count;
                    g_state.sensor.light_history_min[current_day % 7] = light_min;
                    g_state.sensor.light_day_count++;
                    g_state.sensor.light_accum_sec_today = 0;
                    g_state.sensor.active_load_accum_sec_today = 0;
                    g_state.sensor.mqtt_sched_received_today = false;
                    save_needed = true;
                    trigger_schedule_publish = true;

                    Serial.printf("[Rollover] Midnight. Saved active load: %u min. Days: %u\n",
                                  light_min, (unsigned)g_state.sensor.light_day_count);

                    // Re-cache today's sessions at midnight
                    uint8_t today_idx = schedule_get_day_of_week();
                    if (today_idx < SCHEDULE_DAYS && g_state.sensor.sched_weekly.valid) {
                        g_state.sensor.sched_today_sessions_bitmask = g_state.sensor.sched_weekly.day_mask[today_idx];
                        g_state.sensor.sched_active_session_count = 0;
                        memset(g_state.sensor.sched_active_sessions, 0, sizeof(g_state.sensor.sched_active_sessions));
                        for (uint8_t s = 0; s < SCHEDULE_SESSION_COUNT; s++) {
                            bool active = (g_state.sensor.sched_today_sessions_bitmask & (1 << s)) != 0;
                            g_state.sensor.sched_active_sessions[s] = active;
                            if (active) g_state.sensor.sched_active_session_count++;
                        }
                        g_state.sensor.sched_last_triggered_min = 0;
                        g_state.sensor.sched_retry_pending = false;
                        g_state.sensor.sched_retry_check_ms = 0;
                        g_state.sensor.sched_retry_session = 0;
                        g_state.sensor.sched_pre_start_triggered_mask = 0;
                        g_state.sensor.sched_start_triggered_mask = 0;
                        Serial.printf("[Schedule] Midnight refresh: today=%s bitmask=%02X sessions=%u\n",
                                      schedule_get_day_name(today_idx),
                                      g_state.sensor.sched_today_sessions_bitmask,
                                      g_state.sensor.sched_active_session_count);
                    }
                }

                // --- SCHEDULE SHUTDOWN COUNTDOWN ---
                if (g_state.sensor.sched_shutdown_active && millis() >= g_state.sensor.sched_shutdown_timer_ms) {
                    bool presence_valid = g_state.rs485.dashboard.human_presence_valid;
                    bool occupied = presence_valid && g_state.sensor.human_presence;

                    if (presence_valid && !occupied) {
                        g_state.sensor.ac_on = false;
                        g_state.sensor.light_on = false;
                        g_state.sensor.sched_shutdown_active = false;
                        g_state.sensor.sched_shutdown_timer_ms = 0;
                        g_state.ui_needs_update = true;

                        trigger_rs485_ac_off = true;
                        trigger_rs485_light_off = true;
                        target_temp = g_state.sensor.temp_target;
                        fan_speed = g_state.sensor.ac_fan_speed;
                        swing_mode = g_state.sensor.ac_swing_mode;
                        Serial.println("[Schedule] Shutdown timer expired - turning off AC & lights");
                    } else {
                        // Recheck in 5 minutes if still occupied
                        g_state.sensor.sched_shutdown_timer_ms = millis() + (5 * 60 * 1000);
                        g_state.ui_needs_update = true;
                    }
                }

                // --- SESSION-BASED SCHEDULE CHECK ---
                if (g_state.sensor.sched_weekly.valid) {
                    uint8_t today_bitmask = g_state.sensor.sched_today_sessions_bitmask;

                    // Check if any session is currently active (within any class window)
                    uint8_t active_session_now = 255; // which session we're in
                    uint8_t next_session = 255;       // upcoming session

                    for (uint8_t s = 0; s < SCHEDULE_SESSION_COUNT; s++) {
                        if (!(today_bitmask & (1 << s))) continue;

                        uint16_t start = schedule_get_session_start_min(s);
                        uint16_t pre_start = schedule_get_pre_start_min(s);
                        uint16_t end = schedule_get_session_end_min(s);

                        if (minute_now >= pre_start && minute_now < end) {
                            class_schedule_active = true;
                            active_session_now = s;
                        }

                        // Find the next session start (for retry)
                        if (next_session == 255 && minute_now < start) {
                            next_session = s;
                        }
                    }

                    // Pre-class trigger: 20 min before session starts
                    for (uint8_t s = 0; s < SCHEDULE_SESSION_COUNT; s++) {
                        if (!(today_bitmask & (1 << s))) continue;

                        uint16_t start = schedule_get_session_start_min(s);
                        uint16_t pre_start = schedule_get_pre_start_min(s);
                        uint16_t end = schedule_get_session_end_min(s);

                        // Check if we're in pre-class window AND haven't triggered yet
                        if (minute_now >= pre_start && minute_now < start) {
                            if (!(g_state.sensor.sched_pre_start_triggered_mask & (1 << s))) {
                                g_state.sensor.sched_pre_start_triggered_mask |= (1 << s);

                                // Turn on AC to 23 degrees and lights
                                g_state.sensor.temp_target = 23.0f;
                                g_state.sensor.ac_on = true;
                                g_state.sensor.light_on = true;
                                g_state.sensor.sched_shutdown_active = false;
                                g_state.sensor.sched_shutdown_timer_ms = 0;
                                g_state.sensor.sched_retry_pending = false;
                                g_state.sensor.sched_retry_check_ms = 0;
                                g_state.sensor.sched_retry_session = 0;
                                g_state.ui_needs_update = true;
                                trigger_schedule_ac_on = true;
                                trigger_schedule_light_on = true;
                                trigger_schedule_publish = true;
                                target_temp = g_state.sensor.temp_target;
                                fan_speed = PRECLASS_FAN_SPEED;  // Low (1)
                                g_state.sensor.ac_fan_speed = PRECLASS_FAN_SPEED;
                                swing_mode = g_state.sensor.ac_swing_mode;

                                uint8_t sh, sm, eh, em;
                                schedule_get_session_time(s, sh, sm, eh, em);
                                Serial.printf("[Schedule] Pre-class trigger S%u (%02u:%02u - %02u:%02u) - AC 23.0C fan=Low\n",
                                              s + 1, sh, sm, eh, em);
                            }
                        }

                        // Session start check: enforce AC/light ON exactly at start time (or if missed)
                        if (minute_now >= start && minute_now < end) {
                            if (!(g_state.sensor.sched_start_triggered_mask & (1 << s))) {
                                g_state.sensor.sched_start_triggered_mask |= (1 << s);

                                if (!g_state.sensor.ac_on || !g_state.sensor.light_on) {
                                    Serial.printf("[Schedule] Session S%u started - enforcing AC/light ON\n", s + 1);
                                    g_state.sensor.ac_on = true;
                                    g_state.sensor.light_on = true;
                                    g_state.sensor.sched_shutdown_active = false;
                                    g_state.sensor.sched_shutdown_timer_ms = 0;
                                    g_state.ui_needs_update = true;
                                    trigger_schedule_ac_on = true;
                                    trigger_schedule_light_on = true;
                                    trigger_schedule_publish = true;
                                    target_temp = g_state.sensor.temp_target;
                                    fan_speed = g_state.sensor.ac_fan_speed;
                                    swing_mode = g_state.sensor.ac_swing_mode;
                                }
                            }
                        }

                        // Session end: start shutdown timer
                        if (minute_now >= end && minute_now < end + 1) {
                            if (!class_schedule_active) {
                                // No other session active -> start shutdown
                                g_state.sensor.sched_shutdown_active = true;
                                g_state.sensor.sched_shutdown_timer_ms = millis();
                                trigger_schedule_publish = true;
                                g_state.ui_needs_update = true;
                                Serial.printf("[Schedule] Session S%u ended (%02u:%02u). Triggering immediate empty-room shutdown check\n",
                                              s + 1,
                                              schedule_get_session_end_min(s) / 60,
                                              schedule_get_session_end_min(s) % 60);
                            }
                        }
                    }

                    // --- RETRY MECHANISM: If a session started but AC/light are off, retry once ---
                    if (g_state.sensor.sched_retry_pending && millis() >= g_state.sensor.sched_retry_check_ms) {
                        uint8_t retry_s = g_state.sensor.sched_retry_session;
                        if (retry_s < SCHEDULE_SESSION_COUNT && (today_bitmask & (1 << retry_s))) {
                            uint16_t start = schedule_get_session_start_min(retry_s);
                            uint16_t end = schedule_get_session_end_min(retry_s);
                            if (minute_now >= start && minute_now < end) {
                                // Still in session window, force ON
                                if (!g_state.sensor.ac_on || !g_state.sensor.light_on) {
                                    Serial.printf("[Schedule] RETRY S%u: forcing AC/light ON (retry attempt)\n", retry_s + 1);
                                    g_state.sensor.ac_on = true;
                                    g_state.sensor.light_on = true;
                                    g_state.sensor.sched_shutdown_active = false;
                                    g_state.sensor.sched_shutdown_timer_ms = 0;
                                    g_state.ui_needs_update = true;
                                    trigger_schedule_ac_on = true;
                                    trigger_schedule_light_on = true;
                                    trigger_schedule_publish = true;
                                    target_temp = g_state.sensor.temp_target;
                                    fan_speed = g_state.sensor.ac_fan_speed;
                                    swing_mode = g_state.sensor.ac_swing_mode;
                                }
                            }
                        }
                        g_state.sensor.sched_retry_pending = false;
                        g_state.sensor.sched_retry_check_ms = 0;
                        g_state.sensor.sched_retry_session = 0;
                    }

                    // Schedule retry: when someone turns off AC/light during class, retry once after 2 min
                    if (class_schedule_active && active_session_now < SCHEDULE_SESSION_COUNT) {
                        bool class_active_but_off = (!g_state.sensor.ac_on || !g_state.sensor.light_on);
                        if (class_active_but_off && !g_state.sensor.sched_retry_pending &&
                            g_state.sensor.sched_last_triggered_min != minute_now) {
                            // Schedule a retry in 2 minutes
                            g_state.sensor.sched_retry_pending = true;
                            g_state.sensor.sched_retry_check_ms = millis() + (2UL * 60UL * 1000UL);
                            g_state.sensor.sched_retry_session = active_session_now;
                            Serial.printf("[Schedule] Session S%u active but AC/light off - will retry in 2 min\n",
                                          active_session_now + 1);
                        }
                    }
                }

                // --- OLD LEGACY SCHEDULE (backward compat) ---
                if (g_state.sensor.schedule_date_yyyymmdd == today && g_state.sensor.schedule_slot_count > 0) {
                    uint8_t slot_count = g_state.sensor.schedule_slot_count;
                    if (slot_count > DAILY_SCHEDULE_MAX_SLOTS) slot_count = DAILY_SCHEDULE_MAX_SLOTS;
                    for (uint8_t i = 0; i < slot_count; i++) {
                        DailyScheduleSlot& slot = g_state.sensor.schedule_slots[i];
                        uint16_t pre_min = slot.start_min >= 20 ? slot.start_min - 20 : 0;
                        if (!slot.pre_triggered && minute_now >= pre_min && minute_now < slot.start_min) {
                            slot.pre_triggered = true;
                            if (!g_state.sensor.ac_on || !g_state.sensor.light_on) {
                                Serial.println("[Schedule Legacy] Pre-class recovery trigger");
                                g_state.sensor.ac_on = true;
                                g_state.sensor.light_on = true;
                                g_state.sensor.sched_shutdown_active = false;
                                g_state.sensor.sched_shutdown_timer_ms = 0;
                                g_state.ui_needs_update = true;
                                trigger_schedule_ac_on = true;
                                trigger_schedule_light_on = true;
                                trigger_schedule_publish = true;
                                target_temp = g_state.sensor.temp_target;
                                fan_speed = g_state.sensor.ac_fan_speed;
                                swing_mode = g_state.sensor.ac_swing_mode;
                            }
                        }
                        if (minute_now >= slot.end_min && !slot.end_triggered) {
                            slot.end_triggered = true;
                            // Only start shutdown if no new session from new schedule is active
                            if (!class_schedule_active) {
                                g_state.sensor.sched_shutdown_active = true;
                                g_state.sensor.sched_shutdown_timer_ms = millis();
                                trigger_schedule_publish = true;
                                Serial.println("[Schedule Legacy] Slot ended; triggering immediate empty-room shutdown check");
                            }
                        }
                    }
                }

                // --- ACTIVE-LOAD ANOMALY DETECTION ---
                bool presence_valid = g_state.rs485.dashboard.human_presence_valid;
                bool confirmed_empty = presence_valid && !g_state.sensor.human_presence;
                if (g_state.sensor.light_day_count >= 5 && presence_valid) {
                    uint32_t sum_min = 0;
                    uint8_t valid_days = (g_state.sensor.light_day_count >= 7) ? 7 : g_state.sensor.light_day_count;
                    for (uint8_t i = 0; i < valid_days; i++) {
                        sum_min += g_state.sensor.light_history_min[i];
                    }
                    float avg_min = (valid_days > 0) ? ((float)sum_min / (float)valid_days) : 0.0f;
                    float today_min = g_state.sensor.active_load_accum_sec_today / 60.0f;
                    bool after_hours = (timeinfo.tm_hour >= 22 || timeinfo.tm_hour < 6);
                    float threshold_min = avg_min * 1.5f;
                    float plus_60_min = avg_min + 60.0f;
                    if (threshold_min < plus_60_min) threshold_min = plus_60_min;

                    bool old_alert = g_state.sensor.light_anomaly_alert;
                    g_state.sensor.light_anomaly_alert =
                        (avg_min >= 30.0f) &&
                        (today_min > threshold_min) &&
                        after_hours &&
                        confirmed_empty;
                    if (old_alert != g_state.sensor.light_anomaly_alert) {
                        g_state.ui_needs_update = true;
                        save_needed = true;
                    }
                } else {
                    g_state.sensor.light_anomaly_alert = false;
                }

                // --- LED LUX CHECK: Detect dead/missing light tube ---
                // After 15s warmup, if lux < 50lx → CHK LAMP warning.
                // Warning clears immediately when lux rises back to >= 50lx.
                // No baseline comparison — absolute threshold only.
                {
                    static bool prev_light_on_lux = false;
                    static bool led_check_warmup_done = false;
                    bool light_now = g_state.sensor.light_on;

                    if (!prev_light_on_lux && light_now) {
                        // Light just turned ON — start warmup window, clear any old warning
                        g_state.sensor.led_check_start_ms = millis();
                        led_check_warmup_done = false;
                        if (g_state.sensor.led_check_warning) {
                            g_state.sensor.led_check_warning = false;
                            trigger_led_warning_changed = true;
                        }
                        Serial.println("[LED Check] Light ON — 15s warmup started.");
                    } else if (!light_now && prev_light_on_lux) {
                        // Light turned OFF — clear warning & reset
                        bool was_warning = g_state.sensor.led_check_warning;
                        g_state.sensor.led_check_warning = false;
                        g_state.sensor.led_check_start_ms = 0;
                        led_check_warmup_done = false;
                        if (was_warning) {
                            trigger_led_warning_changed = true;
                            g_state.ui_needs_update = true;
                        }
                        Serial.println("[LED Check] Light OFF — warning cleared.");
                    } else if (light_now && g_state.sensor.led_check_start_ms != 0) {
                        if (!led_check_warmup_done) {
                            if ((millis() - g_state.sensor.led_check_start_ms) >= LED_CHECK_WINDOW_MS) {
                                led_check_warmup_done = true;
                                Serial.println("[LED Check] Warmup done. Monitoring lux >= 50lx.");
                            }
                        }
                        if (led_check_warmup_done && g_state.rs485.dashboard.lux_valid) {
                            // Simple absolute check: lux < 50lx = lamp problem
                            bool should_warn = (g_state.rs485.dashboard.lux < LED_CHECK_MIN_DELTA_LUX);
                            if (should_warn != g_state.sensor.led_check_warning) {
                                g_state.sensor.led_check_warning = should_warn;
                                trigger_led_warning_changed = true;
                                g_state.ui_needs_update = true;
                                Serial.printf("[LED Check] WARNING %s (lux=%.1f, threshold=%.1f)\n",
                                              should_warn ? "ACTIVE" : "CLEARED",
                                              g_state.rs485.dashboard.lux,
                                              LED_CHECK_MIN_DELTA_LUX);
                            }
                        }
                    }
                    prev_light_on_lux = light_now;
                }

                // --- AUTO-OFF 5-MINUTE TIMER: Room empty, no class, no shutdown ---
                // Applies regardless of HOW device was turned on (App, Touch, Schedule).
                presence_valid = g_state.rs485.dashboard.human_presence_valid;
                bool occupied = presence_valid && g_state.sensor.human_presence;
                bool any_device_on = g_state.sensor.ac_on ||
                                     g_state.sensor.light_on ||
                                     g_state.sensor.projector_on;
                bool auto_off_conditions = !class_schedule_active &&
                                           !g_state.sensor.sched_shutdown_active &&
                                           presence_valid && !occupied;

                // Detect newly turned-on device to reset an already-running timer
                {
                    static bool prev_ac = false, prev_light = false, prev_proj = false;
                    bool newly_on = (!prev_ac && g_state.sensor.ac_on) ||
                                   (!prev_light && g_state.sensor.light_on) ||
                                   (!prev_proj && g_state.sensor.projector_on);
                    if (newly_on && g_state.sensor.auto_off_pending && auto_off_conditions) {
                        g_state.sensor.auto_off_countdown_ms = millis() + AUTO_OFF_DELAY_MS;
                        g_state.ui_needs_update = true;
                        Serial.println("[Auto-Off] Timer reset — new device activated");
                    }
                    prev_ac    = g_state.sensor.ac_on;
                    prev_light = g_state.sensor.light_on;
                    prev_proj  = g_state.sensor.projector_on;
                }

                if (!auto_off_conditions || !any_device_on) {
                    // Reset countdown if conditions no longer met
                    if (g_state.sensor.auto_off_pending) {
                        g_state.sensor.auto_off_pending = false;
                        g_state.sensor.auto_off_countdown_ms = 0;
                        g_state.ui_needs_update = true;
                        Serial.println("[Auto-Off] Timer cancelled (conditions cleared)");
                    }
                } else if (!g_state.sensor.auto_off_pending) {
                    // Start 5-minute countdown
                    g_state.sensor.auto_off_pending = true;
                    g_state.sensor.auto_off_countdown_ms = millis() + AUTO_OFF_DELAY_MS;
                    g_state.ui_needs_update = true;
                    Serial.println("[Auto-Off] 5-minute countdown started (room empty, no class)");
                } else if ((int32_t)(millis() - g_state.sensor.auto_off_countdown_ms) >= 0) {
                    // Timer expired — turn everything off
                    if (g_state.sensor.ac_on) {
                        g_state.sensor.ac_on = false;
                        trigger_auto_off_ac = true;
                        target_temp = g_state.sensor.temp_target;
                        fan_speed   = g_state.sensor.ac_fan_speed;
                        swing_mode  = g_state.sensor.ac_swing_mode;
                        Serial.println("[Auto-Off] AC off (5-min timer expired)");
                    }
                    if (g_state.sensor.light_on) {
                        g_state.sensor.light_on = false;
                        trigger_auto_off_light = true;
                        Serial.println("[Auto-Off] Light off (5-min timer expired)");
                    }
                    if (g_state.sensor.projector_on) {
                        g_state.sensor.projector_on = false;
                        trigger_auto_off_projector = true;
                        Serial.println("[Auto-Off] Projector off (5-min timer expired)");
                    }
                    g_state.sensor.auto_off_pending = false;
                    g_state.sensor.auto_off_countdown_ms = 0;
                    g_state.ui_needs_update = true;
                }
            }

            data_unlock(g_state);

            // Trigger commands outside lock
            if (trigger_rs485_ac_off || trigger_auto_off_ac) {
                rs485_request_ac_command(false, target_temp, 0, fan_speed, swing_mode);
            }
            if (trigger_rs485_light_off || trigger_auto_off_light) {
                rs485_request_light_command(false);
            }
            if (trigger_auto_off_projector) {
                rs485_request_projector_command(false);
            }
            if (trigger_schedule_ac_on) {
                rs485_request_ac_command(true, target_temp, 0, fan_speed, swing_mode);
            }
            if (trigger_schedule_light_on) {
                rs485_request_light_command(true);
            }
            if (trigger_fan_escalation) {
                // Fan escalation: send AC command with new max fan speed
                data_lock(g_state);
                float esc_temp = g_state.sensor.temp_target;
                uint8_t esc_swing = g_state.sensor.ac_swing_mode;
                data_unlock(g_state);
                rs485_request_ac_command(true, esc_temp, 0, 3, esc_swing);
            }
            if (trigger_rs485_ac_off || trigger_rs485_light_off || trigger_schedule_publish ||
                trigger_auto_off_ac || trigger_auto_off_light || trigger_auto_off_projector ||
                ac_performance_warning_changed || trigger_fan_escalation ||
                trigger_led_warning_changed || save_needed) {
                if (save_needed) {
                    data_save_device_config(g_state);
                }
                mqtt_publish_state();
            }
        }

#undef Serial
        static uint32_t last_hb = 0;
        if (now - last_hb > 5000) {
            last_hb = now;
            if (g_serial_log_mode == LOG_NET) {
                Serial.printf("[NET] Alive | Prio:%d | MQTT:%s | Heap:%u | Stack:%u\n",
                              current_prio,
                              is_mqtt_connected() ? "OK" : "FAIL",
                              (unsigned)ESP.getFreeHeap(),
                              (unsigned)uxTaskGetStackHighWaterMark(NULL));
            }
        }
#define Serial if (g_serial_log_mode == LOG_DATA) Serial

        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Task_Touch: Core 1 — XPT2046 SPI Resistive Touch Polling (20ms / 50Hz)
// ─────────────────────────────────────────────────────────────────────────────
void Task_Touch(void* pvParameters) {
    int tx, ty;
    TouchEventType event;
    for (;;) {
        if (touch_get_event(tx, ty, event)) {
            screens_handle_touch_event(g_state, tx, ty, event);
            data_lock(g_state);
            g_state.ui_needs_update = true;
            data_unlock(g_state);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Task_UI: Core 1 — LovyanGFX Sprite Render Engine (High Priority)
// ─────────────────────────────────────────────────────────────────────────────
#undef Serial

void Task_UI(void* pvParameters) {
    int      frames        = 0;
    int      current_fps   = 0;
    uint32_t last_time     = millis();
    uint32_t last_frame_ts = 0;
    uint32_t total_render  = 0;
    uint32_t total_push    = 0;

    const int target_frame_ms = 25; // ~40 FPS

    for (;;) {
        uint32_t now = millis();

        bool needs_update = false;
        data_lock(g_state);
        needs_update = g_state.ui_needs_update;
        static uint32_t last_force = 0;
        if (now - last_force > 2000) { needs_update = true; last_force = now; }
        data_unlock(g_state);
        if (screens_has_animation()) needs_update = true;

        if (needs_update && (now - last_frame_ts >= (uint32_t)target_frame_ms)) {
            last_frame_ts = now;

            if (xSemaphoreTake(bus_mutex, pdMS_TO_TICKS(target_frame_ms)) == pdTRUE) {
                uint32_t t0 = micros();
                screens_render(g_state, current_fps);
                total_render += (micros() - t0);

                t0 = micros();
                p_engine->pushToDisplay();
                total_push += (micros() - t0);

                widgets_swap();
                xSemaphoreGive(bus_mutex);
                frames++;

                data_lock(g_state);
                g_state.ui_needs_update = false;
                data_unlock(g_state);
            }
        }

        if (millis() - last_time >= 1000) {
            current_fps = frames;
            uint32_t avg_r = (frames > 0) ? (total_render / frames / 1000) : 0;
            uint32_t avg_p = (frames > 0) ? (total_push   / frames / 1000) : 0;
            if (g_serial_log_mode == LOG_CALIB) {
                Serial.printf("[UI] FPS: %d | Render: %dms | Push: %dms\n",
                              current_fps, avg_r, avg_p);
            }
            frames = 0; total_render = 0; total_push = 0;
            last_time = millis();
        }

        vTaskDelay(1);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Serial CLI Task & Menu Implementation
// ─────────────────────────────────────────────────────────────────────────────
const char* serial_log_mode_name(SerialLogMode mode) {
    switch (mode) {
        case LOG_SILENT: return "SILENT";
        case LOG_MQTT:   return "MQTT MONITORING";
        case LOG_NET:    return "NETWORK MONITORING";
        case LOG_DATA:   return "SENSOR DATA MONITORING";
        case LOG_CALIB:  return "TOUCH & DISPLAY CALIBRATION";
        case LOG_RS485:  return "RS485 MODBUS POLLING";
        default:         return "UNKNOWN";
    }
}

void serial_cli_print_menu() {
    Serial.println("\n==================================================");
    Serial.println("       SMART BUILDING S3 MASTER SERIAL CLI");
    Serial.println("==================================================");
    Serial.printf("Current Log Mode: [%s]\n", serial_log_mode_name(g_serial_log_mode));
    
    // Print current MAC mode status
    bool is_spoofed = false;
    int current_prio = 0;
    data_lock(g_state);
    is_spoofed = g_state.net.lan_mac_spoof;
    current_prio = g_state.net.net_priority;
    data_unlock(g_state);
    Serial.printf("LAN MAC Mode:     [%s]\n", is_spoofed ? "SPOOFED (00:50:56:C0:00:01)" : "DEFAULT ESP32 MAC");
    Serial.printf("Network Priority: [%s]\n\n", current_prio == 0 ? "WIFI" : "LAN");

    Serial.println("[1] Monitor MQTT (Status, Tx/Rx Data)");
    Serial.println("[2] Monitor Network Connectivity (WiFi & LAN Status)");
    Serial.println("[3] Monitor Sensor Data (Ambil Data Mode)");
    Serial.println("[4] Monitor Display & Touch Diagnostics (Calibration/Raw)");
    Serial.println("[5] Monitor RS485 Modbus Polling Packets");
    Serial.println("[6] Toggle LAN MAC Address (Spoof vs Default)");
    Serial.println("[7] Toggle Network Priority (WiFi vs LAN)");
    Serial.println("[0] Silent Mode (Mute logs)");
    Serial.println("\n[Enter] Redraw Menu");
    Serial.println("==================================================");
    Serial.print("Enter option (0-7): ");
}

void serial_cli_print_sensor_data() {
    data_lock(g_state);
    float t0 = g_state.sensor.temp[0];
    float t1 = g_state.sensor.temp[1];
    float t2 = g_state.sensor.temp[2];
    float t3 = g_state.sensor.temp[3];
    int co2 = g_state.sensor.co2;
    float lux = g_state.sensor.lux;
    bool presence = g_state.sensor.human_presence;
    bool ac = g_state.sensor.ac_on;
    bool light = g_state.sensor.light_on;
    bool proj = g_state.sensor.projector_on;
    data_unlock(g_state);

    Serial.printf("[DATA] Temp: [T1=%.1f C, T2=%.1f C, T3=%.1f C, T4=%.1f C] | CO2: %d ppm | Lux: %.1f | Presence: %s | AC: %s | Light: %s | Proj: %s\n",
                  t0, t1, t2, t3,
                  co2,
                  lux,
                  presence ? "YES" : "NO",
                  ac ? "ON" : "OFF",
                  light ? "ON" : "OFF",
                  proj ? "ON" : "OFF");
}

void Task_SerialCLI(void* pvParameters) {
    // Wait for boot prints to settle
    vTaskDelay(pdMS_TO_TICKS(1500));
    serial_cli_print_menu();

    uint32_t last_sensor_print_ms = 0;
    SerialLogMode prev_mode = LOG_SILENT;
    uint32_t last_digit_ms = 0;

    for (;;) {
        // Print sensor data periodically if in LOG_DATA mode
        if (g_serial_log_mode == LOG_DATA) {
            uint32_t now = millis();
            if (now - last_sensor_print_ms >= 2000) {
                last_sensor_print_ms = now;
                serial_cli_print_sensor_data();
            }
        }

        // Handle transitions when g_serial_log_mode changes
        if (g_serial_log_mode != prev_mode) {
            // Exit cleanup of previous mode
            if (prev_mode == LOG_DATA) {
                data_lock(g_state);
                g_state.sensor.data_collect_mode = false;
                data_unlock(g_state);
                data_save_device_config(g_state);
                Serial.println("\n[SYSTEM] Mode Ambil Data: NON-AKTIF (Mode Biasa)");
            } else if (prev_mode == LOG_CALIB) {
                screens_set(SCREEN_DASHBOARD);
                Serial.println("\n[UI] Returned to dashboard.");
            }

            // Entry action of new mode
            if (g_serial_log_mode == LOG_DATA) {
                data_lock(g_state);
                g_state.sensor.data_collect_mode = true;
                data_unlock(g_state);
                data_save_device_config(g_state);
                Serial.println("\n[SYSTEM] Mode Ambil Data: AKTIF (Kirim 10s)");
            } else if (g_serial_log_mode == LOG_CALIB) {
                screens_set(SCREEN_TOUCH_TEST);
                Serial.println("\n[UI] Touch alignment test opened on display.");
            }

            prev_mode = g_serial_log_mode;
        }

        // Check for serial input
        while (Serial.available() > 0) {
            char c = Serial.read();
            if (c == '\r' || c == '\n' || c == 'm' || c == 'M' || c == 'h' || c == 'H' || c == 'q' || c == 'Q') {
                static uint32_t last_enter_ms = 0;
                uint32_t now = millis();
                if (now - last_digit_ms > 250) { // debounce: ignore enter key if pressed immediately after a number choice
                    if (now - last_enter_ms > 100) {
                        last_enter_ms = now;
                        if (g_serial_log_mode != LOG_SILENT) {
                            g_serial_log_mode = LOG_SILENT;
                            Serial.println("\n>>> Returning to Main Menu <<<");
                            serial_cli_print_menu();
                        } else {
                            serial_cli_print_menu();
                        }
                    }
                }
            } else if (c >= '0' && c <= '7') {
                last_digit_ms = millis();
                if (c == '6') {
                    data_lock(g_state);
                    g_state.net.lan_mac_spoof = !g_state.net.lan_mac_spoof;
                    bool new_val = g_state.net.lan_mac_spoof;
                    data_unlock(g_state);

                    lan_manager_save_config(); // Saves to NVS and triggers LAN restart!

                    Serial.printf("\n>>> LAN MAC Mode changed to: %s <<<\n", new_val ? "SPOOFED (00:50:56:C0:00:01)" : "DEFAULT (ESP32 MAC)");
                    Serial.println("(Ethernet controller is restarting in the background...)\n");
                    serial_cli_print_menu();
                } else if (c == '7') {
                    data_lock(g_state);
                    g_state.net.net_priority = (g_state.net.net_priority == 0) ? 1 : 0;
                    int new_prio = g_state.net.net_priority;
                    data_unlock(g_state);

                    // Re-apply power config (e.g. disable/enable WiFi radio accordingly)
                    wifi_manager_set_power(new_prio == 0);

                    Serial.printf("\n>>> Network Priority changed to: %s <<<\n", new_prio == 0 ? "WIFI" : "LAN (Ethernet)");
                    if (new_prio == 1) {
                        Serial.println("(Ethernet controller is initializing in the background...)\n");
                    }
                    serial_cli_print_menu();
                } else {
                    SerialLogMode target_mode = (SerialLogMode)(c - '0');
                    if (target_mode == LOG_SILENT) {
                        g_serial_log_mode = LOG_SILENT;
                        Serial.println("\n>>> Silent Mode Enabled <<<");
                        serial_cli_print_menu();
                    } else {
                        g_serial_log_mode = target_mode;
                        Serial.printf("\n>>> Log Mode changed to: %s <<<\n", serial_log_mode_name(target_mode));
                        Serial.println("(Press [Enter] or '0' to return to Main Menu)\n");
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// setup() — Boot sequence
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n[BOOT] Smart Building Master S3 — Serial SPI Edition");

    display_init();   // LovyanGFX ILI9488 init
    data_init(g_state);
    apply_dummy_rs485_slave(ENABLE_DUMMY_RS485_SLAVE == 1);

    // UI Event Callbacks (decouples UI from app logic)
    UIEventCallbacks ui_cbs;
    ui_cbs.onWiFiConnect    = [](const char* s, const char* p) { wifi_manager_connect(s, p); };
    ui_cbs.onWiFiReconnect  = []() { wifi_manager_reconnect(); };
    ui_cbs.onWiFiScan       = []() { wifi_manager_scan_request(); };
    ui_cbs.onLANSave        = []() { lan_manager_save_config(); };
    ui_cbs.onPriorityChange = [](int p) { wifi_manager_set_power(p == 0); };
    ui_cbs.onRS485Pairing   = []() { rs485_request_pairing(); };
    ui_cbs.onRS485PairingCancel = []() { rs485_cancel_pairing(); };
    ui_cbs.onRS485PairingAssign = [](uint8_t address) { rs485_request_assign_pairing_candidate(address); };
    ui_cbs.onRS485PollToggle = [](bool enabled) { rs485_set_poll_enabled(enabled); };
    ui_cbs.onRS485Test = [](uint8_t address, uint8_t cmd, bool write_command) {
        rs485_request_test(address, cmd, write_command);
    };

    screens_init(ui_cbs);  // Also calls widgets_init() which creates LGFX sprites
    touch_init();          // XPT2046 resistive SPI touch diagnostics

    rs485_task_init();     // Task_RS485 pinned to Core 0

    xTaskCreatePinnedToCore(Task_Net,   "Task_Net",   8192,  NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(Task_Touch, "Task_Touch", 4096,  NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(Task_UI,    "Task_UI",    16384, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(Task_SerialCLI, "Serial_CLI", 4096,  NULL, 1, NULL, 1);
}

void loop() {
    vTaskDelete(NULL); // loop() task killed — all work is in RTOS tasks
}
