#ifndef DATA_H
#define DATA_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define WIFI_SCAN_MAX_RESULTS 16
#define RS485_MAX_SLAVES 8
#define DASHBOARD_TEMP_SLOTS 4
#define DASHBOARD_LOGICAL_SLOT_COUNT 9
#define RS485_DUMMY_UI_UID 0xD00D0001UL
#define DAILY_SCHEDULE_MAX_SLOTS 8

struct DailyScheduleSlot {
    uint16_t start_min;
    uint16_t end_min;
    bool     pre_triggered;
    bool     end_triggered;
};

enum CapabilityBit : uint16_t {
    CAP_TEMP           = (1 << 0),
    CAP_CO2            = (1 << 1),
    CAP_HUMAN_PRESENCE = (1 << 2),
    CAP_AC_IR          = (1 << 3),
    CAP_PROJECTOR_IR   = (1 << 4),
    CAP_LIGHT_RELAY    = (1 << 5),
    CAP_LUX            = (1 << 6),
    CAP_LCD_CTRL       = (1 << 7)
};

enum DashboardLogicalId : uint8_t {
    LOGICAL_TEMP_SLOT_1 = 0,
    LOGICAL_TEMP_SLOT_2,
    LOGICAL_TEMP_SLOT_3,
    LOGICAL_TEMP_SLOT_4,
    LOGICAL_CO2_MAIN,
    LOGICAL_LUX_MAIN,
    LOGICAL_HUMAN_PRESENCE_MAIN,
    LOGICAL_AC_CONTROL,
    LOGICAL_PROJECTOR_CONTROL
};

enum DeviceProfile : uint8_t {
    DEVICE_PROFILE_UNASSIGNED = 0,
    TEMP_NODE,
    PRESENCE_NODE,
    CO2_NODE,
    RELAY_NODE,
    IR_COMBO_NODE
};

enum DeviceRegistryStatus : uint8_t {
    DEVICE_STATUS_UNKNOWN = 0,
    DEVICE_STATUS_ONLINE,
    DEVICE_STATUS_OFFLINE,
    DEVICE_STATUS_DEGRADED,
    DEVICE_STATUS_UNPAIRED_DEVICE_DETECTED
};

struct LogicalMapping {
    uint8_t  logical_id;
    uint8_t  capability_type;
    uint32_t slave_uid;
    uint8_t  slave_addr;
    uint8_t  channel;
    bool     assigned;
    bool     manual_override;
};

struct DashboardModel {
    float temp[DASHBOARD_TEMP_SLOTS];
    bool  temp_valid[DASHBOARD_TEMP_SLOTS];
    int   co2;
    bool  co2_valid;
    float lux;
    bool  lux_valid;
    float lux_channel[4];
    bool  lux_channel_valid[4];
    bool  human_presence;
    bool  human_presence_valid;
    bool  ac_available;
    bool  projector_available;
    bool  projector_on;
    uint8_t proj_verif_state;
    bool  proj_hw_fail;
    bool  led_on;
};

struct WiFiScanResult {
    char    ssid[33];
    int32_t rssi;
    uint8_t encryption;
    uint8_t channel;
};

struct RS485SlaveState {
    uint8_t  address;
    uint64_t mac;
    uint32_t uid;
    char     name[24];
    char     room[24];
    DeviceProfile profile;
    DeviceRegistryStatus registry_status;
    uint8_t  role;
    uint16_t capability;
    uint16_t enabled_mask;
    uint16_t protocol_version;
    uint16_t device_class;
    uint16_t fw_version;
    uint8_t  temp_count;
    uint8_t  temp_available_mask;
    uint8_t  temp_enabled_mask;
    uint8_t  co2_count;
    uint8_t  presence_count;
    uint8_t  relay_count;
    uint8_t  ir_count;
    uint8_t  lux_count;
    uint8_t  lcd_count;
    uint16_t relay_state[2];
    float    temp[DASHBOARD_TEMP_SLOTS];
    bool     temp_valid[DASHBOARD_TEMP_SLOTS];
    int      co2;
    bool     co2_valid;
    float    lux;
    bool     lux_valid;
    float    lux_channel[4];
    bool     lux_channel_valid[4];
    bool     human_presence;
    bool     human_presence_valid;
    bool     identity_synced;
    bool     capability_synced;
    uint32_t last_identity_ms;
    uint32_t last_capability_ms;
    uint32_t last_seen;
    bool     sensor_poll_pending;
    bool     online;
    bool     degraded;
    uint16_t error_count;
    uint16_t rx_success;
    uint16_t crc_errors;
    uint16_t timeout_errors;
    uint16_t seq_errors;
    uint16_t len_errors;
    uint16_t nack_count;
    uint8_t  consecutive_fail;
};

struct RS485State {
    bool initialized;
    bool bus_ok;
    bool pairing_requested;
    bool pairing_active;
    bool pairing_candidate_ready;
    bool pairing_assign_requested;
    bool poll_enabled;
    uint8_t pairing_assign_address;
    uint32_t pairing_started_ms;
    uint32_t pairing_timeout_ms;
    uint16_t pairing_timeouts;
    uint8_t slave_count;
    uint32_t packets_tx;
    uint32_t packets_rx;
    uint32_t crc_errors;
    uint32_t timeout_errors;
    bool test_requested;
    bool test_busy;
    bool test_write;
    bool test_ok;
    bool light_command_requested;
    bool light_command_on;
    uint8_t light_command_channel;
    bool light_state_publish_pending;
    bool light_command_failed;
    bool ac_command_requested;
    bool ac_command_power;
    float ac_command_target_c;
    uint8_t ac_command_mode;
    uint8_t ac_command_fan_speed;
    uint8_t ac_command_swing_mode;
    bool projector_command_requested;
    bool projector_command_power;
    uint8_t projector_command_input;
    uint8_t test_address;
    uint8_t test_cmd;
    uint8_t test_result;
    uint8_t test_seq;
    uint8_t test_attempts;
    uint32_t test_started_ms;
    uint32_t test_done_ms;
    char test_status[64];
    char status[64];
    RS485SlaveState pairing_candidate;
    RS485SlaveState slaves[RS485_MAX_SLAVES];
    LogicalMapping mappings[DASHBOARD_LOGICAL_SLOT_COUNT];
    DashboardModel dashboard;
};

struct SensorData {
    float    temp[4];
    float    temp_target;
    float    lux;
    int      co2;
    bool     ac_on;
    uint8_t  ac_fan_speed;
    uint8_t  ac_swing_mode;
    bool     ac_performance_warning;
    bool     ac_performance_monitor_active;
    float    ac_performance_start_temp_c;
    float    ac_performance_target_c;
    uint32_t ac_performance_started_ms;
    bool     projector_on;
    bool     light_on;
    bool     human_presence;
    bool     sensor_error[4];
    uint8_t  slave_count;
    bool     slave_online[2];

    // Projector Lux Verification fields
    uint8_t  proj_verif_state;       // 0=OFF, 1=POWERING_ON, 2=VERIFIED_ON, 3=RETRYING, 4=NO_LUX, 5=CHECK_LUX, 6=CHECK_PROJECTOR
    float    proj_lux_initial;
    float    proj_lux_baseline_avg;
    bool     proj_lux_baseline_valid;
    float    proj_lux_baseline[4];
    bool     proj_lux_baseline_channel_valid[4];
    uint32_t proj_lux_source_key;
    uint32_t proj_warmup_timer_ms;
    uint32_t proj_warning_until_ms;
    uint8_t  proj_retry_count;
    bool     proj_hardware_failed;

    // Scheduler fields
    bool     sched_shutdown_active;
    uint32_t sched_shutdown_timer_ms;
    uint32_t schedule_date_yyyymmdd;
    uint8_t  schedule_slot_count;
    DailyScheduleSlot schedule_slots[DAILY_SCHEDULE_MAX_SLOTS];

    // Rolling light history fields
    uint32_t light_on_start_ms;
    uint32_t light_accum_sec_today;
    uint32_t active_load_accum_sec_today;
    uint16_t light_history_min[7];
    uint32_t light_day_count;
    bool     light_anomaly_alert;
};

struct NetworkState {
    bool  wifi_connected;
    bool  lan_connected;
    bool  lan_initialized;
    bool  lan_dhcp_ok;
    bool  lan_static_fallback;
    bool  lan_checking;
    bool  firebase_ok;
    bool  mqtt_ok;
    int   net_priority;           // 0=WiFi, 1=LAN
    bool  lan_use_dhcp;
    char  lan_ip[16];
    char  lan_current_gateway[16];
    char  lan_current_subnet[16];
    char  lan_current_dns[16];
    char  lan_link_status[24];
    char  connected_wifi_ssid[32];
    char  saved_wifi_ssid[32];
    char  saved_wifi_pass[64];
    bool  time_synced;
    bool  time_syncing;
    char  time_source[8];
    char  time_status[40];
    char  lan_static_ip[16];
    char  lan_gateway[16];
    char  lan_subnet[16];
    char  lan_dns[16];
    char  time_str[16];
    char  room_name[32];
    char  device_name[32];
    char  class_name[16];
    char  mqtt_server[64];
    uint16_t mqtt_port;
    bool  mqtt_use_tls;
    char  mqtt_user[32];
    char  mqtt_pass[64];
    char  slave_name[2][32];
    char  conn_status[32];
    char  lan_status_detail[64];
    char  wifi_status_detail[64];
    bool  wifi_scan_requested;
    bool  wifi_scan_active;
    bool  wifi_scan_start_pending;
    bool  wifi_scan_radio_warming;
    bool  wifi_scan_done;
    bool  wifi_scan_error;
    bool  wifi_scan_has_results;
    uint8_t wifi_scan_count;
    uint32_t wifi_scan_requested_ts;
    uint32_t wifi_scan_started_ts;
    uint32_t wifi_scan_finished_ts;
    uint8_t wifi_scan_start_attempts;
    char  wifi_scan_status[64];
    WiFiScanResult wifi_scan_results[WIFI_SCAN_MAX_RESULTS];
};

struct BuildingState {
    SensorData          sensor;
    NetworkState        net;
    int                 dashboard_page;
    bool                use_dummy;
    bool                ui_needs_update;
    uint32_t            last_data_ts;
    SemaphoreHandle_t   mutex;
    RS485State          rs485;

    // Touch diagnostics
    int                 touch_x;
    int                 touch_y;
    uint16_t            touch_raw_x;
    uint16_t            touch_raw_y;
    bool                touch_pressed;
    int                 touch_last_x;
    int                 touch_last_y;
    uint16_t            touch_last_raw_x;
    uint16_t            touch_last_raw_y;
};

extern BuildingState g_state;

void data_init(BuildingState& state);
void data_load_dummy(BuildingState& state);
void data_load_device_config(BuildingState& state);
void data_save_device_config(BuildingState& state);
void data_load_rs485_config(BuildingState& state);
void data_save_rs485_config(BuildingState& state);
void data_lock(BuildingState& state);
void data_unlock(BuildingState& state);
const char* device_profile_name(DeviceProfile profile);
const char* device_registry_status_name(DeviceRegistryStatus status);
uint16_t device_profile_capability_mask(DeviceProfile profile);
DeviceProfile device_profile_from_capabilities(uint16_t capability);

#endif
