#include "data.h"
#include "mapping_manager.h"
#include <Preferences.h>
#include <string.h>
#include <time.h>

#if __has_include("mqtt_secrets.h")
#include "mqtt_secrets.h"
#endif
#include "mqtt_defaults.h"

BuildingState g_state;

static const char* RS485_PREF_NS = "rs485cfg";
static const char* DEVICE_PREF_NS = "device_cfg";
static const uint8_t RS485_PREF_ASSIGN_SCHEMA = 2;

// ────────────────────────────────────────────────────────────────────────────
// BINUS Session Time Definitions (S1..S6)
// Each session = 100 minutes, gap = 20 minutes between sessions
// ────────────────────────────────────────────────────────────────────────────
static const SessionConfig SESSION_TIMES[SCHEDULE_SESSION_COUNT] = {
    { 7, 20,  9,  0 },  // S1: 07:20 - 09:00
    { 9, 20, 11,  0 },  // S2: 09:20 - 11:00
    { 11, 20, 13, 0 },  // S3: 11:20 - 13:00
    { 13, 20, 15, 0 },  // S4: 13:20 - 15:00
    { 15, 20, 17, 0 },  // S5: 15:20 - 17:00
    { 17, 20, 19, 0 }   // S6: 17:20 - 19:00
};

void schedule_get_session_time(uint8_t session_index, uint8_t& start_hour, uint8_t& start_min,
                                uint8_t& end_hour, uint8_t& end_min) {
    if (session_index >= SCHEDULE_SESSION_COUNT) session_index = 0;
    start_hour = SESSION_TIMES[session_index].start_hour;
    start_min  = SESSION_TIMES[session_index].start_min;
    end_hour   = SESSION_TIMES[session_index].end_hour;
    end_min    = SESSION_TIMES[session_index].end_min;
}

uint16_t schedule_get_session_start_min(uint8_t session_index) {
    if (session_index >= SCHEDULE_SESSION_COUNT) session_index = 0;
    return (uint16_t)SESSION_TIMES[session_index].start_hour * 60U + SESSION_TIMES[session_index].start_min;
}

uint16_t schedule_get_session_end_min(uint8_t session_index) {
    if (session_index >= SCHEDULE_SESSION_COUNT) session_index = 0;
    return (uint16_t)SESSION_TIMES[session_index].end_hour * 60U + SESSION_TIMES[session_index].end_min;
}

uint16_t schedule_get_pre_start_min(uint8_t session_index) {
    uint16_t start = schedule_get_session_start_min(session_index);
    return start >= 20 ? start - 20 : 0;
}

uint8_t schedule_get_day_of_week() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 5)) return 255;
    // tm_wday: 0=Sunday..6=Saturday, we need 0=Monday..6=Sunday
    int wday = timeinfo.tm_wday; // 0=Sun
    if (wday == 0) return 6;     // Sunday -> index 6
    return wday - 1;             // Mon=0, Tue=1, ..., Sat=5
}

bool schedule_is_session_active(const WeeklyScheduleData& wsd, uint8_t day_index, uint8_t session_index) {
    if (!wsd.valid || day_index >= SCHEDULE_DAYS || session_index >= SCHEDULE_SESSION_COUNT) return false;
    return (wsd.day_mask[day_index] & (1 << session_index)) != 0;
}

uint8_t schedule_get_active_sessions_today(const WeeklyScheduleData& wsd) {
    uint8_t day = schedule_get_day_of_week();
    if (day >= SCHEDULE_DAYS || !wsd.valid) return 0;
    return wsd.day_mask[day];
}

const char* schedule_get_day_name(uint8_t day_index) {
    static const char* day_names[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
    if (day_index >= SCHEDULE_DAYS) return "Unknown";
    return day_names[day_index];
}

const char* device_profile_name(DeviceProfile profile) {
    switch (profile) {
        case TEMP_NODE: return "TEMP_NODE";
        case PRESENCE_NODE: return "PRESENCE_NODE";
        case CO2_NODE: return "CO2_NODE";
        case RELAY_NODE: return "RELAY_NODE";
        case IR_COMBO_NODE: return "IR_COMBO_NODE";
        default: return "UNASSIGNED";
    }
}

const char* device_registry_status_name(DeviceRegistryStatus status) {
    switch (status) {
        case DEVICE_STATUS_ONLINE: return "ONLINE";
        case DEVICE_STATUS_OFFLINE: return "OFFLINE";
        case DEVICE_STATUS_DEGRADED: return "DEGRADED";
        case DEVICE_STATUS_UNPAIRED_DEVICE_DETECTED: return "UNPAIRED_DEVICE_DETECTED";
        default: return "UNKNOWN";
    }
}

uint16_t device_profile_capability_mask(DeviceProfile profile) {
    switch (profile) {
        case TEMP_NODE: return CAP_TEMP | CAP_LUX;
        case PRESENCE_NODE: return CAP_HUMAN_PRESENCE | CAP_LUX;
        case CO2_NODE: return CAP_CO2 | CAP_LUX;
        case RELAY_NODE: return CAP_LIGHT_RELAY | CAP_LUX;
        case IR_COMBO_NODE: return CAP_AC_IR | CAP_PROJECTOR_IR | CAP_LUX;
        default: return 0;
    }
}

DeviceProfile device_profile_from_capabilities(uint16_t capability) {
    if (capability & (CAP_AC_IR | CAP_PROJECTOR_IR)) return IR_COMBO_NODE;
    if (capability & CAP_LIGHT_RELAY) return RELAY_NODE;
    if (capability & CAP_CO2) return CO2_NODE;
    if (capability & CAP_HUMAN_PRESENCE) return PRESENCE_NODE;
    if (capability & CAP_TEMP) return TEMP_NODE;
    return DEVICE_PROFILE_UNASSIGNED;
}

void data_init(BuildingState& state) {
    state.mutex = xSemaphoreCreateMutex();
    state.use_dummy = false;
    state.net.lan_mac_spoof = false;
    state.ui_needs_update = true;
    state.last_data_ts = millis();

    state.touch_x = -1;
    state.touch_y = -1;
    state.touch_raw_x = 0;
    state.touch_raw_y = 0;
    state.touch_pressed = false;
    state.touch_last_x = -1;
    state.touch_last_y = -1;
    state.touch_last_raw_x = 0;
    state.touch_last_raw_y = 0;

    data_load_dummy(state);
    data_load_device_config(state);
    data_load_rs485_config(state);
}

void data_load_dummy(BuildingState& state) {
    if (xSemaphoreTake(state.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        state.sensor.temp[0] = -100.0f;
        state.sensor.temp[1] = -100.0f;
        state.sensor.temp[2] = -100.0f;
        state.sensor.temp[3] = -100.0f;
        state.sensor.temp_target = 23.0f;
        state.sensor.lux = -1.0f;
        state.sensor.co2 = -1;
        state.sensor.ac_on = true;
        state.sensor.ac_fan_speed = 0;
        state.sensor.ac_swing_mode = 0;
        state.sensor.ac_performance_warning = false;
        state.sensor.ac_performance_monitor_active = false;
        state.sensor.ac_performance_start_temp_c = -100.0f;
        state.sensor.ac_performance_target_c = state.sensor.temp_target;
        state.sensor.ac_performance_started_ms = 0;
        state.sensor.projector_on = false;
        state.sensor.light_on = true;
        state.sensor.human_presence = true;
        memset(state.sensor.sensor_error, 0, sizeof(state.sensor.sensor_error));
        state.sensor.slave_count = 2;
        state.sensor.slave_online[0] = true;
        state.sensor.slave_online[1] = true;

        state.sensor.proj_verif_state = 0;
        state.sensor.proj_lux_initial = -1.0f;
        state.sensor.proj_lux_baseline_avg = -1.0f;
        state.sensor.proj_lux_baseline_valid = false;
        memset(state.sensor.proj_lux_baseline, 0, sizeof(state.sensor.proj_lux_baseline));
        memset(state.sensor.proj_lux_baseline_channel_valid, 0, sizeof(state.sensor.proj_lux_baseline_channel_valid));
        state.sensor.proj_lux_source_key = 0;
        state.sensor.proj_warmup_timer_ms = 0;
        state.sensor.proj_warning_until_ms = 0;
        state.sensor.proj_retry_count = 0;
        state.sensor.proj_hardware_failed = false;

        state.sensor.sched_shutdown_active = false;
        state.sensor.sched_shutdown_timer_ms = 0;
        state.sensor.sched_weekly.valid = false;
        strncpy(state.sensor.last_mqtt_sched_payload, "-", sizeof(state.sensor.last_mqtt_sched_payload));
        state.sensor.mqtt_sched_received_today = false;
        memset(state.sensor.sched_weekly.day_mask, 0, sizeof(state.sensor.sched_weekly.day_mask));
        memset(state.sensor.sched_active_sessions, 0, sizeof(state.sensor.sched_active_sessions));
        state.sensor.sched_active_session_count = 0;
        state.sensor.sched_today_sessions_bitmask = 0;
        state.sensor.sched_last_triggered_min = 0;
        state.sensor.sched_retry_pending = false;
        state.sensor.sched_retry_check_ms = 0;
        state.sensor.sched_retry_session = 0;
        state.sensor.sched_pre_start_triggered_mask = 0;
        state.sensor.sched_start_triggered_mask = 0;
        state.sensor.schedule_date_yyyymmdd = 0;
        state.sensor.schedule_slot_count = 0;
        memset(state.sensor.schedule_slots, 0, sizeof(state.sensor.schedule_slots));

        state.sensor.light_on_start_ms = 0;
        state.sensor.light_accum_sec_today = 0;
        state.sensor.active_load_accum_sec_today = 0;
        memset(state.sensor.light_history_min, 0, sizeof(state.sensor.light_history_min));
        state.sensor.light_day_count = 0;
        state.sensor.light_anomaly_alert = false;
        state.sensor.data_collect_mode = false;
        state.sensor.app_controlled_ac = false;
        state.sensor.app_controlled_light = false;
        state.sensor.app_controlled_projector = false;
        state.sensor.auto_off_pending = false;
        state.sensor.auto_off_countdown_ms = 0;
        state.sensor.led_check_warning = false;
        state.sensor.led_check_start_ms = 0;
        state.sensor.led_check_baseline_lux = -1.0f;
        state.sensor.ac_fan_escalated = false;

        state.net.wifi_connected = true;
        state.net.lan_connected  = false;
        state.net.lan_initialized = false;
        state.net.lan_dhcp_ok = false;
        state.net.lan_static_fallback = false;
        state.net.lan_checking = false;
        state.net.firebase_ok    = true;
        state.net.mqtt_ok        = false;
        state.net.net_priority   = 0;
        state.net.lan_use_dhcp   = true;
        state.dashboard_page = 0;

        strcpy(state.net.time_str,   "--:--");
        strcpy(state.net.room_name,  "Meeting Room A");
        strcpy(state.net.device_name, "Meeting Room Master");
        strcpy(state.net.class_name, "HD01");
        strcpy(state.net.mqtt_server, MQTT_SERVER_DEFAULT);
        state.net.mqtt_port = MQTT_PORT_SECURE_DEFAULT;
        state.net.mqtt_use_tls = true;
        strncpy(state.net.mqtt_user, MQTT_USER_DEFAULT, sizeof(state.net.mqtt_user) - 1);
        state.net.mqtt_user[sizeof(state.net.mqtt_user) - 1] = '\0';
        strncpy(state.net.mqtt_pass, MQTT_PASS_DEFAULT, sizeof(state.net.mqtt_pass) - 1);
        state.net.mqtt_pass[sizeof(state.net.mqtt_pass) - 1] = '\0';
        strcpy(state.net.slave_name[0], "Slave 1");
        strcpy(state.net.slave_name[1], "Slave 2");
        strcpy(state.net.conn_status,        "Initializing...");
        strcpy(state.net.lan_status_detail,  "Not initialized");
        strcpy(state.net.wifi_status_detail, "Not connected");
        strcpy(state.net.wifi_scan_status,   "Tap REFRESH to scan");
        strcpy(state.net.lan_ip, "-");
        state.net.time_synced = false;
        state.net.time_syncing = false;
        strcpy(state.net.time_source, "-");
        strcpy(state.net.time_status, "Time not synced");
        strcpy(state.net.lan_current_gateway, "-");
        strcpy(state.net.lan_current_subnet, "-");
        strcpy(state.net.lan_current_dns, "-");
        strcpy(state.net.lan_link_status, "Unknown");
        strcpy(state.net.lan_static_ip, "192.168.1.177");
        strcpy(state.net.lan_gateway,   "192.168.1.1");
        strcpy(state.net.lan_subnet,    "255.255.255.0");
        strcpy(state.net.lan_dns,       "8.8.8.8");
        strcpy(state.net.connected_wifi_ssid, "-");
        state.net.saved_wifi_ssid[0] = '\0';
        state.net.saved_wifi_pass[0] = '\0';
        state.net.use_manual_time = false;

        state.net.wifi_scan_requested = false;
        state.net.wifi_scan_active = false;
        state.net.wifi_scan_start_pending = false;
        state.net.wifi_scan_radio_warming = false;
        state.net.wifi_scan_done = false;
        state.net.wifi_scan_error = false;
        state.net.wifi_scan_has_results = false;
        state.net.wifi_scan_count = 0;
        state.net.wifi_scan_requested_ts = 0;
        state.net.wifi_scan_started_ts = 0;
        state.net.wifi_scan_finished_ts = 0;
        state.net.wifi_scan_start_attempts = 0;
        memset(state.net.wifi_scan_results, 0, sizeof(state.net.wifi_scan_results));

        state.rs485.initialized = false;
        state.rs485.bus_ok = false;
        state.rs485.pairing_requested = false;
        state.rs485.pairing_active = false;
        state.rs485.pairing_candidate_ready = false;
        state.rs485.pairing_assign_requested = false;
        state.rs485.poll_enabled = true;
        state.rs485.pairing_assign_address = 0;
        state.rs485.pairing_started_ms = 0;
        state.rs485.pairing_timeout_ms = 0;
        state.rs485.pairing_timeouts = 0;
        state.rs485.slave_count = 1;
        state.rs485.packets_tx = 0;
        state.rs485.packets_rx = 0;
        state.rs485.crc_errors = 0;
        state.rs485.timeout_errors = 0;
        state.rs485.test_requested = false;
        state.rs485.test_busy = false;
        state.rs485.test_write = false;
        state.rs485.test_ok = false;
        state.rs485.light_command_requested = false;
        state.rs485.light_command_on = false;
        state.rs485.light_command_channel = 0;
        state.rs485.light_state_publish_pending = false;
        state.rs485.light_command_failed = false;
        state.rs485.ac_command_requested = false;
        state.rs485.ac_command_power = false;
        state.rs485.ac_command_target_c = state.sensor.temp_target;
        state.rs485.ac_command_mode = 0;
        state.rs485.ac_command_fan_speed = state.sensor.ac_fan_speed;
        state.rs485.ac_command_swing_mode = state.sensor.ac_swing_mode;
        state.rs485.projector_command_requested = false;
        state.rs485.projector_command_power = false;
        state.rs485.projector_command_input = 0;
        state.rs485.test_address = 0x00;
        state.rs485.test_cmd = 0x03;
        state.rs485.test_result = 0;
        state.rs485.test_seq = 0;
        state.rs485.test_attempts = 0;
        state.rs485.test_started_ms = 0;
        state.rs485.test_done_ms = 0;
        strcpy(state.rs485.status, "RS485 not initialized");
        strcpy(state.rs485.test_status, "No manual test yet");
        memset(&state.rs485.pairing_candidate, 0, sizeof(state.rs485.pairing_candidate));
        memset(state.rs485.slaves, 0, sizeof(state.rs485.slaves));
        state.rs485.slaves[0].address = 0x00;
        strcpy(state.rs485.slaves[0].name, "Device 0");
        strcpy(state.rs485.slaves[0].room, "Unassigned");
        state.rs485.slaves[0].profile = DEVICE_PROFILE_UNASSIGNED;
        state.rs485.slaves[0].registry_status = DEVICE_STATUS_UNKNOWN;
        state.rs485.slaves[0].role = 0x00;
        state.rs485.slaves[0].enabled_mask = 0;
        state.rs485.slaves[0].temp_available_mask = 0;
        state.rs485.slaves[0].temp_enabled_mask = 0;

        for (uint8_t i = 0; i < DASHBOARD_LOGICAL_SLOT_COUNT; i++) {
            state.rs485.mappings[i].logical_id = i;
            state.rs485.mappings[i].capability_type = 0;
            state.rs485.mappings[i].slave_uid = 0;
            state.rs485.mappings[i].slave_addr = 0;
            state.rs485.mappings[i].channel = 0;
            state.rs485.mappings[i].assigned = false;
            state.rs485.mappings[i].manual_override = false;
        }
        state.rs485.mappings[LOGICAL_TEMP_SLOT_1].capability_type = CAP_TEMP;
        state.rs485.mappings[LOGICAL_TEMP_SLOT_2].capability_type = CAP_TEMP;
        state.rs485.mappings[LOGICAL_TEMP_SLOT_3].capability_type = CAP_TEMP;
        state.rs485.mappings[LOGICAL_TEMP_SLOT_4].capability_type = CAP_TEMP;
        state.rs485.mappings[LOGICAL_CO2_MAIN].capability_type = CAP_CO2;
        state.rs485.mappings[LOGICAL_LUX_MAIN].capability_type = CAP_LUX;
        state.rs485.mappings[LOGICAL_HUMAN_PRESENCE_MAIN].capability_type = CAP_HUMAN_PRESENCE;
        state.rs485.mappings[LOGICAL_AC_CONTROL].capability_type = CAP_AC_IR;
        state.rs485.mappings[LOGICAL_PROJECTOR_CONTROL].capability_type = CAP_PROJECTOR_IR;

        for (uint8_t i = 0; i < DASHBOARD_TEMP_SLOTS; i++) {
            state.rs485.dashboard.temp[i] = -100.0f;
            state.rs485.dashboard.temp_valid[i] = false;
        }
        state.rs485.dashboard.co2 = -1;
        state.rs485.dashboard.co2_valid = false;
        state.rs485.dashboard.lux = -1.0f;
        state.rs485.dashboard.lux_valid = false;
        memset(state.rs485.dashboard.lux_channel, 0, sizeof(state.rs485.dashboard.lux_channel));
        memset(state.rs485.dashboard.lux_channel_valid, 0, sizeof(state.rs485.dashboard.lux_channel_valid));
        state.rs485.dashboard.human_presence = false;
        state.rs485.dashboard.human_presence_valid = false;
        state.rs485.dashboard.ac_available = false;
        state.rs485.dashboard.projector_available = false;

        xSemaphoreGive(state.mutex);
    }
}

void data_load_device_config(BuildingState& state) {
    Preferences prefs;
    if (!prefs.begin(DEVICE_PREF_NS, true)) return;

    data_lock(state);
    prefs.getString("device_name", state.net.device_name, sizeof(state.net.device_name));
    prefs.getString("class_name", state.net.class_name, sizeof(state.net.class_name));
    prefs.getString("mqtt_server", state.net.mqtt_server, sizeof(state.net.mqtt_server));
    state.net.mqtt_port = prefs.getUShort("mqtt_port", state.net.mqtt_port);
    state.net.mqtt_use_tls = prefs.getBool("mqtt_tls", state.net.mqtt_use_tls);
    prefs.getString("mqtt_user", state.net.mqtt_user, sizeof(state.net.mqtt_user));
    prefs.getString("mqtt_pass", state.net.mqtt_pass, sizeof(state.net.mqtt_pass));
    state.net.use_manual_time = prefs.getBool("man_time", false);
    if (state.net.device_name[0] == '\0') {
        strncpy(state.net.device_name, "Meeting Room Master", sizeof(state.net.device_name) - 1);
        state.net.device_name[sizeof(state.net.device_name) - 1] = '\0';
    }
    if (state.net.class_name[0] == '\0') {
        strncpy(state.net.class_name, "HD01", sizeof(state.net.class_name) - 1);
        state.net.class_name[sizeof(state.net.class_name) - 1] = '\0';
    }
    if (state.net.mqtt_server[0] == '\0') {
        strncpy(state.net.mqtt_server, MQTT_SERVER_DEFAULT, sizeof(state.net.mqtt_server) - 1);
        state.net.mqtt_server[sizeof(state.net.mqtt_server) - 1] = '\0';
    }
    if (state.net.mqtt_port == 0) {
        state.net.mqtt_port = state.net.mqtt_use_tls ? MQTT_PORT_SECURE_DEFAULT : MQTT_PORT_NORMAL_DEFAULT;
    }

    state.sensor.light_day_count = prefs.getUInt("l_day_cnt", 0);
    state.sensor.light_accum_sec_today = prefs.getUInt("l_acc_sec", 0);
    state.sensor.active_load_accum_sec_today = prefs.getUInt("al_acc_sec", state.sensor.light_accum_sec_today);
    state.sensor.light_anomaly_alert = prefs.getBool("l_anom_alrt", false);
    state.sensor.data_collect_mode = prefs.getBool("data_coll", false);
    for (int i = 0; i < 7; i++) {
        char key[16];
        snprintf(key, sizeof(key), "l_hist_%d", i);
        state.sensor.light_history_min[i] = prefs.getUShort(key, 0);
    }
    // Load NEW weekly schedule format
    state.sensor.sched_weekly.valid = prefs.getBool("sw_valid", false);
    for (uint8_t d = 0; d < SCHEDULE_DAYS; d++) {
        char key[16];
        snprintf(key, sizeof(key), "sw_day%u", d);
        state.sensor.sched_weekly.day_mask[d] = prefs.getUChar(key, 0);
    }
    prefs.getString("mq_sch_pay", state.sensor.last_mqtt_sched_payload, sizeof(state.sensor.last_mqtt_sched_payload));
    if (strlen(state.sensor.last_mqtt_sched_payload) == 0) {
        strncpy(state.sensor.last_mqtt_sched_payload, "-", sizeof(state.sensor.last_mqtt_sched_payload));
    }
    state.sensor.mqtt_sched_received_today = prefs.getBool("mq_sch_rcv", false);
    memset(state.sensor.sched_active_sessions, 0, sizeof(state.sensor.sched_active_sessions));
    state.sensor.sched_active_session_count = 0;
    state.sensor.sched_today_sessions_bitmask = 0;
    state.sensor.sched_last_triggered_min = 0;
    state.sensor.sched_retry_pending = false;
    state.sensor.sched_retry_check_ms = 0;
    state.sensor.sched_retry_session = 0;

    // Legacy load (for backward compat only)
    state.sensor.schedule_date_yyyymmdd = prefs.getUInt("sched_date", 0);
    state.sensor.schedule_slot_count = prefs.getUChar("sched_count", 0);
    if (state.sensor.schedule_slot_count > DAILY_SCHEDULE_MAX_SLOTS) {
        state.sensor.schedule_slot_count = 0;
    }
    for (uint8_t i = 0; i < DAILY_SCHEDULE_MAX_SLOTS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "sched_s%u", i);
        state.sensor.schedule_slots[i].start_min = prefs.getUShort(key, 0);
        snprintf(key, sizeof(key), "sched_e%u", i);
        state.sensor.schedule_slots[i].end_min = prefs.getUShort(key, 0);
        state.sensor.schedule_slots[i].pre_triggered = false;
        state.sensor.schedule_slots[i].end_triggered = false;
    }

    state.ui_needs_update = true;
    data_unlock(state);

    prefs.end();
}

void data_save_device_config(BuildingState& state) {
    Preferences prefs;
    if (!prefs.begin(DEVICE_PREF_NS, false)) return;

    data_lock(state);
    prefs.putString("device_name", state.net.device_name);
    prefs.putString("class_name", state.net.class_name);
    prefs.putString("mqtt_server", state.net.mqtt_server);
    prefs.putUShort("mqtt_port", state.net.mqtt_port);
    prefs.putBool("mqtt_tls", state.net.mqtt_use_tls);
    prefs.putString("mqtt_user", state.net.mqtt_user);
    prefs.putString("mqtt_pass", state.net.mqtt_pass);
    prefs.putBool("man_time", state.net.use_manual_time);

    prefs.putUInt("l_day_cnt", state.sensor.light_day_count);
    prefs.putUInt("l_acc_sec", state.sensor.light_accum_sec_today);
    prefs.putUInt("al_acc_sec", state.sensor.active_load_accum_sec_today);
    prefs.putBool("l_anom_alrt", state.sensor.light_anomaly_alert);
    prefs.putBool("data_coll", state.sensor.data_collect_mode);
    for (int i = 0; i < 7; i++) {
        char key[16];
        snprintf(key, sizeof(key), "l_hist_%d", i);
        prefs.putUShort(key, state.sensor.light_history_min[i]);
    }
    // Save NEW weekly schedule
    prefs.putBool("sw_valid", state.sensor.sched_weekly.valid);
    for (uint8_t d = 0; d < SCHEDULE_DAYS; d++) {
        char key[16];
        snprintf(key, sizeof(key), "sw_day%u", d);
        prefs.putUChar(key, state.sensor.sched_weekly.day_mask[d]);
    }
    prefs.putString("mq_sch_pay", state.sensor.last_mqtt_sched_payload);
    prefs.putBool("mq_sch_rcv", state.sensor.mqtt_sched_received_today);

    // Legacy save (backward compat)
    prefs.putUInt("sched_date", state.sensor.schedule_date_yyyymmdd);
    prefs.putUChar("sched_count", state.sensor.schedule_slot_count);
    for (uint8_t i = 0; i < DAILY_SCHEDULE_MAX_SLOTS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "sched_s%u", i);
        prefs.putUShort(key, state.sensor.schedule_slots[i].start_min);
        snprintf(key, sizeof(key), "sched_e%u", i);
        prefs.putUShort(key, state.sensor.schedule_slots[i].end_min);
    }

    data_unlock(state);

    prefs.end();
}

void data_load_rs485_config(BuildingState& state) {
    Preferences prefs;
    if (!prefs.begin(RS485_PREF_NS, true)) return;
    uint8_t assign_schema = prefs.getUChar("assign_schema", 0);

    data_lock(state);

    uint8_t slave_count = prefs.getUChar("slave_count", state.rs485.slave_count);
    if (slave_count > RS485_MAX_SLAVES) slave_count = RS485_MAX_SLAVES;
    if (slave_count == 0) slave_count = 1;
    state.rs485.slave_count = slave_count;

    for (uint8_t i = 0; i < RS485_MAX_SLAVES; i++) {
        char key[20];
        snprintf(key, sizeof(key), "s%u_addr", i);
        state.rs485.slaves[i].address = prefs.getUChar(key, state.rs485.slaves[i].address);
        snprintf(key, sizeof(key), "s%u_uid", i);
        state.rs485.slaves[i].uid = prefs.getULong(key, state.rs485.slaves[i].uid);
        snprintf(key, sizeof(key), "s%u_mac", i);
        state.rs485.slaves[i].mac = prefs.getULong64(key, state.rs485.slaves[i].mac);
        snprintf(key, sizeof(key), "s%u_prof", i);
        state.rs485.slaves[i].profile = (DeviceProfile)prefs.getUChar(key, state.rs485.slaves[i].profile);
        if (state.rs485.slaves[i].profile > IR_COMBO_NODE) {
            state.rs485.slaves[i].profile = DEVICE_PROFILE_UNASSIGNED;
        }
        snprintf(key, sizeof(key), "s%u_cap", i);
        state.rs485.slaves[i].capability = prefs.getUShort(key, state.rs485.slaves[i].capability);
        snprintf(key, sizeof(key), "s%u_en", i);
        state.rs485.slaves[i].enabled_mask = prefs.getUShort(key, state.rs485.slaves[i].enabled_mask);
        if (assign_schema < RS485_PREF_ASSIGN_SCHEMA) {
            state.rs485.slaves[i].enabled_mask = 0;
        }
        snprintf(key, sizeof(key), "s%u_tc", i);
        state.rs485.slaves[i].temp_count = prefs.getUChar(key, state.rs485.slaves[i].temp_count);
        snprintf(key, sizeof(key), "s%u_tam", i);
        state.rs485.slaves[i].temp_available_mask = prefs.getUChar(key, state.rs485.slaves[i].temp_available_mask);
        if (state.rs485.slaves[i].temp_available_mask == 0 && state.rs485.slaves[i].temp_count > 0) {
            uint8_t mask = 0;
            uint8_t count = state.rs485.slaves[i].temp_count;
            if (count > DASHBOARD_TEMP_SLOTS) count = DASHBOARD_TEMP_SLOTS;
            for (uint8_t ch = 0; ch < count; ch++) mask |= (1 << ch);
            state.rs485.slaves[i].temp_available_mask = mask;
        }
        snprintf(key, sizeof(key), "s%u_tem", i);
        state.rs485.slaves[i].temp_enabled_mask = prefs.getUChar(key, state.rs485.slaves[i].temp_enabled_mask);
        if (assign_schema < RS485_PREF_ASSIGN_SCHEMA) {
            state.rs485.slaves[i].temp_enabled_mask = 0;
        }
        if (state.rs485.slaves[i].temp_available_mask == 0 && state.rs485.slaves[i].temp_enabled_mask != 0) {
            state.rs485.slaves[i].temp_available_mask = state.rs485.slaves[i].temp_enabled_mask & 0x0F;
            state.rs485.slaves[i].temp_count = 0;
            for (uint8_t ch = 0; ch < DASHBOARD_TEMP_SLOTS; ch++) {
                if (state.rs485.slaves[i].temp_available_mask & (1 << ch)) state.rs485.slaves[i].temp_count++;
            }
        }
        snprintf(key, sizeof(key), "s%u_cc", i);
        state.rs485.slaves[i].co2_count = prefs.getUChar(key, state.rs485.slaves[i].co2_count);
        snprintf(key, sizeof(key), "s%u_pc", i);
        state.rs485.slaves[i].presence_count = prefs.getUChar(key, state.rs485.slaves[i].presence_count);
        snprintf(key, sizeof(key), "s%u_lxc", i);
        state.rs485.slaves[i].lux_count = prefs.getUChar(key, state.rs485.slaves[i].lux_count);
        snprintf(key, sizeof(key), "s%u_rc", i);
        state.rs485.slaves[i].relay_count = prefs.getUChar(key, state.rs485.slaves[i].relay_count);
        snprintf(key, sizeof(key), "s%u_ic", i);
        state.rs485.slaves[i].ir_count = prefs.getUChar(key, state.rs485.slaves[i].ir_count);
        snprintf(key, sizeof(key), "s%u_lcdc", i);
        state.rs485.slaves[i].lcd_count = prefs.getUChar(key, state.rs485.slaves[i].lcd_count);
        snprintf(key, sizeof(key), "s%u_name", i);
        prefs.getString(key, state.rs485.slaves[i].name, sizeof(state.rs485.slaves[i].name));
        if (state.rs485.slaves[i].name[0] == '\0' && state.rs485.slaves[i].address != 0) {
            snprintf(state.rs485.slaves[i].name, sizeof(state.rs485.slaves[i].name), "Node %02X", state.rs485.slaves[i].address);
        }
        snprintf(key, sizeof(key), "s%u_room", i);
        prefs.getString(key, state.rs485.slaves[i].room, sizeof(state.rs485.slaves[i].room));
        if (state.rs485.slaves[i].room[0] == '\0') {
            strncpy(state.rs485.slaves[i].room, state.net.class_name[0] ? state.net.class_name : "Room",
                    sizeof(state.rs485.slaves[i].room) - 1);
            state.rs485.slaves[i].room[sizeof(state.rs485.slaves[i].room) - 1] = '\0';
        }
        if (state.rs485.slaves[i].profile == DEVICE_PROFILE_UNASSIGNED) {
            state.rs485.slaves[i].profile = device_profile_from_capabilities(
                state.rs485.slaves[i].enabled_mask ? state.rs485.slaves[i].enabled_mask : state.rs485.slaves[i].capability);
        }
        if (state.rs485.slaves[i].address != 0 || state.rs485.slaves[i].mac != 0) {
            state.rs485.slaves[i].registry_status = state.rs485.slaves[i].online ? DEVICE_STATUS_ONLINE : DEVICE_STATUS_OFFLINE;
        } else {
            state.rs485.slaves[i].registry_status = DEVICE_STATUS_UNKNOWN;
        }
    }

    for (uint8_t i = 0; i < DASHBOARD_LOGICAL_SLOT_COUNT; i++) {
        char key[20];
        snprintf(key, sizeof(key), "m%u_uid", i);
        state.rs485.mappings[i].slave_uid = prefs.getULong(key, state.rs485.mappings[i].slave_uid);
        snprintf(key, sizeof(key), "m%u_addr", i);
        state.rs485.mappings[i].slave_addr = prefs.getUChar(key, state.rs485.mappings[i].slave_addr);
        snprintf(key, sizeof(key), "m%u_ch", i);
        state.rs485.mappings[i].channel = prefs.getUChar(key, state.rs485.mappings[i].channel);
        snprintf(key, sizeof(key), "m%u_asg", i);
        state.rs485.mappings[i].assigned = prefs.getBool(key, state.rs485.mappings[i].assigned);
        snprintf(key, sizeof(key), "m%u_man", i);
        state.rs485.mappings[i].manual_override = prefs.getBool(key, state.rs485.mappings[i].manual_override);
    }

    mapping_manager_update_locked(state);
    state.ui_needs_update = true;
    data_unlock(state);

    prefs.end();
}

void data_save_rs485_config(BuildingState& state) {
    Preferences prefs;
    if (!prefs.begin(RS485_PREF_NS, false)) return;

    data_lock(state);
    prefs.putUChar("assign_schema", RS485_PREF_ASSIGN_SCHEMA);
    prefs.putUChar("slave_count", state.rs485.slave_count);

    for (uint8_t i = 0; i < RS485_MAX_SLAVES; i++) {
        char key[20];
        const RS485SlaveState& slave = state.rs485.slaves[i];
        snprintf(key, sizeof(key), "s%u_addr", i);
        prefs.putUChar(key, slave.address);
        snprintf(key, sizeof(key), "s%u_uid", i);
        prefs.putULong(key, slave.uid);
        snprintf(key, sizeof(key), "s%u_mac", i);
        prefs.putULong64(key, slave.mac);
        snprintf(key, sizeof(key), "s%u_prof", i);
        prefs.putUChar(key, (uint8_t)slave.profile);
        snprintf(key, sizeof(key), "s%u_cap", i);
        prefs.putUShort(key, slave.capability);
        snprintf(key, sizeof(key), "s%u_en", i);
        prefs.putUShort(key, slave.enabled_mask);
        snprintf(key, sizeof(key), "s%u_tc", i);
        prefs.putUChar(key, slave.temp_count);
        snprintf(key, sizeof(key), "s%u_tam", i);
        prefs.putUChar(key, slave.temp_available_mask);
        snprintf(key, sizeof(key), "s%u_tem", i);
        prefs.putUChar(key, slave.temp_enabled_mask);
        snprintf(key, sizeof(key), "s%u_cc", i);
        prefs.putUChar(key, slave.co2_count);
        snprintf(key, sizeof(key), "s%u_pc", i);
        prefs.putUChar(key, slave.presence_count);
        snprintf(key, sizeof(key), "s%u_lxc", i);
        prefs.putUChar(key, slave.lux_count);
        snprintf(key, sizeof(key), "s%u_rc", i);
        prefs.putUChar(key, slave.relay_count);
        snprintf(key, sizeof(key), "s%u_ic", i);
        prefs.putUChar(key, slave.ir_count);
        snprintf(key, sizeof(key), "s%u_lcdc", i);
        prefs.putUChar(key, slave.lcd_count);
        snprintf(key, sizeof(key), "s%u_name", i);
        prefs.putString(key, slave.name);
        snprintf(key, sizeof(key), "s%u_room", i);
        prefs.putString(key, slave.room);
    }

    for (uint8_t i = 0; i < DASHBOARD_LOGICAL_SLOT_COUNT; i++) {
        char key[20];
        const LogicalMapping& mapping = state.rs485.mappings[i];
        snprintf(key, sizeof(key), "m%u_uid", i);
        prefs.putULong(key, mapping.slave_uid);
        snprintf(key, sizeof(key), "m%u_addr", i);
        prefs.putUChar(key, mapping.slave_addr);
        snprintf(key, sizeof(key), "m%u_ch", i);
        prefs.putUChar(key, mapping.channel);
        snprintf(key, sizeof(key), "m%u_asg", i);
        prefs.putBool(key, mapping.assigned);
        snprintf(key, sizeof(key), "m%u_man", i);
        prefs.putBool(key, mapping.manual_override);
    }
    data_unlock(state);

    prefs.end();
}

void data_lock(BuildingState& state) {
    xSemaphoreTake(state.mutex, portMAX_DELAY);
}

void data_unlock(BuildingState& state) {
    xSemaphoreGive(state.mutex);
}

SerialLogMode g_serial_log_mode = LOG_SILENT;
