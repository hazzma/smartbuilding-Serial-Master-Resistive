#include "rs485_manager.h"
#include "data.h"
#include "mapping_manager.h"
#include "ui_screens.h"

#include <DFRobot_RTU.h>
#include <string.h>
#include <strings.h>

static HardwareSerial rs485_serial(RS485_UART_NUM);
static DFRobot_RTU rs485_modbus(&rs485_serial, RS485_DIR_PIN);

static uint8_t next_seq = 1;
static uint32_t last_poll_ms = 0;
static uint32_t last_pairing_scan_ms = 0;
static uint32_t last_auto_recovery_ms[RS485_MAX_SLAVES] = {0};
static uint8_t pairing_known_scan_index = 0;
static uint8_t poll_index = 0;
static bool debug_force_rx_result = false;
static RS485RxResult debug_forced_rx_result = RS485_RX_TIMEOUT;
static RS485TransactionResult last_transaction = {};
static char debug_line[96] = {0};
static uint8_t debug_line_len = 0;

static const uint32_t RS485_RESPONSE_TIMEOUT_MS = 100;
static const uint8_t RS485_RETRY_COUNT = 1;
// One poll interval advances one registry slot, not every slave at once.
// Failed-attempt thresholds count both the initial request and its retry.
static const uint32_t RS485_POLL_INTERVAL_MS = 1000;
static const uint32_t RS485_PAIRING_SCAN_INTERVAL_MS = 700;
static const uint32_t RS485_PAIRING_KNOWN_SCAN_WINDOW_MS = 4000;
static const uint32_t RS485_IDENTITY_SYNC_INTERVAL_MS = 10000;
static const uint32_t RS485_CAPABILITY_SYNC_INTERVAL_MS = 5000;
static const uint32_t RS485_OFFLINE_TIMEOUT_MS = 5000;
static const uint32_t RS485_PAIRING_TIMEOUT_MS = 30000;
// Recovery is rate-limited per saved slave. Normal assigned-address polling
// continues between recovery attempts, so a powered slave reconnects normally.
static const uint32_t RS485_AUTO_RECOVERY_INTERVAL_MS = 10000;
static const uint8_t RS485_DEGRADED_THRESHOLD = 3;
static const uint8_t RS485_OFFLINE_FAIL_THRESHOLD = 5;
static const uint8_t RS485_MODBUS_FC_READ_HOLDING = 0x03;
static const uint8_t RS485_MODBUS_FC_WRITE_SINGLE = 0x06;
static const uint8_t RS485_MODBUS_FC_WRITE_MULTIPLE = 0x10;
static bool pairing_restore_poll_enabled = true;
static uint32_t debug_pairing_timeout_override_ms = 0;

struct ModbusRequestPlan {
    bool write;
    uint8_t function_code;
    uint16_t reg;
    uint16_t quantity;
    uint16_t value;
    const char* label;
};

static bool rs485_poll_sensor_registers(uint8_t address);
bool rs485_apply_slave_assignments(uint8_t slave_index);

static const char* rs485_rx_result_name(RS485RxResult result) {
    switch (result) {
        case RS485_RX_OK: return "OK";
        case RS485_RX_TIMEOUT: return "TIMEOUT";
        case RS485_RX_CRC_ERROR: return "CRC_ERROR";
        case RS485_RX_SEQ_MISMATCH: return "SEQ_MISMATCH";
        case RS485_RX_CMD_MISMATCH: return "CMD_MISMATCH";
        case RS485_RX_ADDR_MISMATCH: return "ADDR_MISMATCH";
        case RS485_RX_NACK: return "NACK";
        case RS485_RX_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

static bool rs485_parse_rx_result(const char* token, RS485RxResult& result) {
    if (!token) return false;
    if (strcasecmp(token, "ok") == 0) result = RS485_RX_OK;
    else if (strcasecmp(token, "timeout") == 0) result = RS485_RX_TIMEOUT;
    else if (strcasecmp(token, "crc") == 0 || strcasecmp(token, "crc_error") == 0) result = RS485_RX_CRC_ERROR;
    else if (strcasecmp(token, "seq") == 0 || strcasecmp(token, "seq_mismatch") == 0) result = RS485_RX_SEQ_MISMATCH;
    else if (strcasecmp(token, "cmd") == 0 || strcasecmp(token, "cmd_mismatch") == 0) result = RS485_RX_CMD_MISMATCH;
    else if (strcasecmp(token, "addr") == 0 || strcasecmp(token, "addr_mismatch") == 0) result = RS485_RX_ADDR_MISMATCH;
    else if (strcasecmp(token, "nack") == 0) result = RS485_RX_NACK;
    else if (strcasecmp(token, "error") == 0) result = RS485_RX_ERROR;
    else return false;
    return true;
}

static void rs485_mark_tx() {
    data_lock(g_state);
    g_state.rs485.packets_tx++;
    data_unlock(g_state);
}

static void rs485_mark_rx() {
    data_lock(g_state);
    g_state.rs485.packets_rx++;
    g_state.rs485.bus_ok = true;
    data_unlock(g_state);
}

static uint8_t rs485_find_slave_index_locked(uint8_t address) {
    uint8_t count = g_state.rs485.slave_count;
    if (count > RS485_MAX_SLAVES) count = RS485_MAX_SLAVES;
    for (uint8_t i = 0; i < count; i++) {
        if (g_state.rs485.slaves[i].address == address) return i;
    }
    return RS485_MAX_SLAVES;
}

static void rs485_inc_u16(uint16_t& value) {
    if (value < UINT16_MAX) value++;
}

static void rs485_mark_slave_success(uint8_t address) {
    data_lock(g_state);
    uint8_t index = rs485_find_slave_index_locked(address);
    if (index < RS485_MAX_SLAVES) {
        RS485SlaveState& slave = g_state.rs485.slaves[index];
        bool was_online = slave.online;
        bool was_degraded = slave.degraded;
        rs485_inc_u16(slave.rx_success);
        slave.consecutive_fail = 0;
        slave.degraded = false;
        slave.online = true;
        slave.last_seen = millis();
        if (index < 2) g_state.sensor.slave_online[index] = true;
        if (!was_online || was_degraded) g_state.ui_needs_update = true;
    }
    snprintf(g_state.rs485.status, sizeof(g_state.rs485.status), "RS485 Modbus OK 0x%02X", address);
    data_unlock(g_state);
}

static void rs485_mark_result_error(uint8_t address, RS485RxResult result) {
    data_lock(g_state);

    if (result == RS485_RX_CRC_ERROR) g_state.rs485.crc_errors++;
    if (result == RS485_RX_TIMEOUT) g_state.rs485.timeout_errors++;

    uint8_t index = rs485_find_slave_index_locked(address);
    if (index < RS485_MAX_SLAVES) {
        RS485SlaveState& slave = g_state.rs485.slaves[index];
        bool was_online = slave.online;
        bool was_degraded = slave.degraded;

        rs485_inc_u16(slave.error_count);
        if (slave.consecutive_fail < UINT8_MAX) slave.consecutive_fail++;

        switch (result) {
            case RS485_RX_CRC_ERROR:
                rs485_inc_u16(slave.crc_errors);
                break;
            case RS485_RX_TIMEOUT:
                rs485_inc_u16(slave.timeout_errors);
                break;
            case RS485_RX_SEQ_MISMATCH:
                rs485_inc_u16(slave.seq_errors);
                break;
            case RS485_RX_NACK:
                rs485_inc_u16(slave.nack_count);
                break;
            case RS485_RX_CMD_MISMATCH:
            case RS485_RX_ADDR_MISMATCH:
            case RS485_RX_ERROR:
                rs485_inc_u16(slave.len_errors);
                break;
            default:
                break;
        }

        if (slave.consecutive_fail >= RS485_DEGRADED_THRESHOLD) slave.degraded = true;
        if (slave.consecutive_fail >= RS485_OFFLINE_FAIL_THRESHOLD ||
            millis() - slave.last_seen > RS485_OFFLINE_TIMEOUT_MS) {
            slave.online = false;
            if (index < 2) g_state.sensor.slave_online[index] = false;
        }

        mapping_manager_update_locked(g_state);
        if (was_online != slave.online || was_degraded != slave.degraded) g_state.ui_needs_update = true;
    }

    snprintf(g_state.rs485.status, sizeof(g_state.rs485.status),
             "RS485 Modbus %s 0x%02X", rs485_rx_result_name(result), address);
    data_unlock(g_state);
}

static RS485RxResult rs485_result_from_modbus_code(uint8_t code) {
    if (code == 0) return RS485_RX_OK;
    if (code == 8) return RS485_RX_CRC_ERROR;
    if (code == 9) return RS485_RX_TIMEOUT;
    if (code == 11) return RS485_RX_ADDR_MISMATCH;
    if (code >= 1 && code <= 4) return RS485_RX_NACK;
    return RS485_RX_ERROR;
}

static uint16_t rs485_payload_u16(const uint8_t* payload, uint8_t len, uint16_t fallback) {
    if (!payload || len == 0) return fallback;
    if (len == 1) return payload[0];
    return ((uint16_t)payload[0] << 8) | payload[1];
}

static uint16_t rs485_payload_register(const uint8_t* payload, uint8_t len, uint8_t index, uint16_t fallback = 0) {
    uint8_t offset = index * 2;
    if (!payload || len < offset + 2) return fallback;
    return ((uint16_t)payload[offset] << 8) | payload[offset + 1];
}

static bool rs485_read_holding_registers(uint8_t dst,
                                         uint16_t reg,
                                         uint16_t quantity,
                                         uint16_t* registers,
                                         const char* label) {
    if (dst == RS485_BROADCAST_ADDR || !registers || quantity == 0) return false;
    if (quantity > (RS485_MAX_PAYLOAD / 2)) quantity = RS485_MAX_PAYLOAD / 2;

    for (uint8_t attempt = 0; attempt <= RS485_RETRY_COUNT; attempt++) {
        while (rs485_serial.available()) rs485_serial.read();
        rs485_mark_tx();
        Serial.printf("[RS485] Modbus sync TX dst=0x%02X reg=0x%04X qty=%u attempt=%u label=%s\n",
                      dst,
                      reg,
                      quantity,
                      attempt,
                      label ? label : "-");

        uint8_t modbus_code = rs485_modbus.readHoldingRegister(dst, reg, registers, quantity);
        RS485RxResult result = rs485_result_from_modbus_code(modbus_code);
        if (result == RS485_RX_OK) {
            rs485_mark_rx();
            rs485_mark_slave_success(dst);
            Serial.printf("[RS485] Modbus sync OK dst=0x%02X reg=0x%04X qty=%u label=%s\n",
                          dst,
                          reg,
                          quantity,
                          label ? label : "-");
            return true;
        }

        Serial.printf("[RS485] Modbus sync %s dst=0x%02X reg=0x%04X code=%u label=%s\n",
                      rs485_rx_result_name(result),
                      dst,
                      reg,
                      modbus_code,
                      label ? label : "-");
        rs485_mark_result_error(dst, result);
    }

    return false;
}

static bool rs485_write_holding_register(uint8_t dst, uint16_t reg, uint16_t value, const char* label) {
    if (dst == RS485_BROADCAST_ADDR) return false;

    for (uint8_t attempt = 0; attempt <= RS485_RETRY_COUNT; attempt++) {
        while (rs485_serial.available()) rs485_serial.read();
        rs485_mark_tx();
        Serial.printf("[RS485] Modbus write TX dst=0x%02X reg=0x%04X value=0x%04X attempt=%u label=%s\n",
                      dst,
                      reg,
                      value,
                      attempt,
                      label ? label : "-");

        uint8_t modbus_code = rs485_modbus.writeHoldingRegister(dst, reg, value);
        RS485RxResult result = rs485_result_from_modbus_code(modbus_code);
        if (result == RS485_RX_OK) {
            rs485_mark_rx();
            rs485_mark_slave_success(dst);
            Serial.printf("[RS485] Modbus write OK dst=0x%02X reg=0x%04X label=%s\n",
                          dst,
                          reg,
                          label ? label : "-");
            return true;
        }

        Serial.printf("[RS485] Modbus write %s dst=0x%02X reg=0x%04X code=%u label=%s\n",
                      rs485_rx_result_name(result),
                      dst,
                      reg,
                      modbus_code,
                      label ? label : "-");
        rs485_mark_result_error(dst, result);
    }

    return false;
}

static bool rs485_write_holding_registers(uint8_t dst,
                                          uint16_t reg,
                                          uint16_t* values,
                                          uint16_t quantity,
                                          const char* label) {
    if (dst == RS485_BROADCAST_ADDR || !values || quantity == 0) return false;
    if (quantity > (RS485_MAX_PAYLOAD / 2)) quantity = RS485_MAX_PAYLOAD / 2;

    for (uint8_t attempt = 0; attempt <= RS485_RETRY_COUNT; attempt++) {
        while (rs485_serial.available()) rs485_serial.read();
        rs485_mark_tx();
        Serial.printf("[RS485] Modbus write-multiple TX dst=0x%02X reg=0x%04X qty=%u attempt=%u label=%s\n",
                      dst,
                      reg,
                      quantity,
                      attempt,
                      label ? label : "-");

        uint8_t modbus_code = rs485_modbus.writeHoldingRegister(dst, reg, values, quantity);
        RS485RxResult result = rs485_result_from_modbus_code(modbus_code);
        if (result == RS485_RX_OK) {
            rs485_mark_rx();
            rs485_mark_slave_success(dst);
            Serial.printf("[RS485] Modbus write-multiple OK dst=0x%02X reg=0x%04X qty=%u label=%s\n",
                          dst,
                          reg,
                          quantity,
                          label ? label : "-");
            return true;
        }

        Serial.printf("[RS485] Modbus write-multiple %s dst=0x%02X reg=0x%04X code=%u label=%s\n",
                      rs485_rx_result_name(result),
                      dst,
                      reg,
                      modbus_code,
                      label ? label : "-");
        rs485_mark_result_error(dst, result);
    }

    return false;
}

static void rs485_store_identity(uint8_t address, const uint16_t* registers, uint8_t count) {
    if (!registers || count < RS485_MODBUS_IDENTITY_REGS) return;

    data_lock(g_state);
    RS485SlaveState* slave_ptr = nullptr;
    uint8_t index = rs485_find_slave_index_locked(address);
    if (index < RS485_MAX_SLAVES) {
        slave_ptr = &g_state.rs485.slaves[index];
    } else if (address == RS485_MODBUS_PAIRING_ADDR) {
        slave_ptr = &g_state.rs485.pairing_candidate;
        g_state.rs485.pairing_candidate.address = RS485_MODBUS_PAIRING_ADDR;
    }

    if (slave_ptr) {
        RS485SlaveState& slave = *slave_ptr;
        slave.protocol_version = registers[RS485_MODBUS_REG_FW_VERSION - RS485_MODBUS_REG_NODE_ADDRESS];
        slave.device_class = 0;
        slave.fw_version = registers[RS485_MODBUS_REG_FW_VERSION - RS485_MODBUS_REG_NODE_ADDRESS];
        uint16_t mac01 = registers[RS485_MODBUS_REG_MAC_0_1 - RS485_MODBUS_REG_NODE_ADDRESS];
        uint16_t mac23 = registers[RS485_MODBUS_REG_MAC_2_3 - RS485_MODBUS_REG_NODE_ADDRESS];
        uint16_t mac45 = registers[RS485_MODBUS_REG_MAC_4_5 - RS485_MODBUS_REG_NODE_ADDRESS];
        slave.mac = ((uint64_t)(mac01 >> 8) << 40) |
                    ((uint64_t)(mac01 & 0xFF) << 32) |
                    ((uint64_t)(mac23 >> 8) << 24) |
                    ((uint64_t)(mac23 & 0xFF) << 16) |
                    ((uint64_t)(mac45 >> 8) << 8) |
                    (uint64_t)(mac45 & 0xFF);
        slave.uid = slave.mac != 0 ? (uint32_t)(slave.mac & 0xFFFFFFFFUL) : 0;
        if (slave.name[0] == '\0') {
            snprintf(slave.name, sizeof(slave.name), "Node %02X", address);
        }
        slave.identity_synced = true;
        slave.last_identity_ms = millis();
        mapping_manager_update_locked(g_state);
        snprintf(g_state.rs485.status, sizeof(g_state.rs485.status),
                 "RS485 identity 0x%02X", address);
        g_state.ui_needs_update = true;
    }
    data_unlock(g_state);
}

static uint8_t rs485_popcount4(uint16_t value) {
    value &= 0x000F;
    uint8_t count = 0;
    for (uint8_t i = 0; i < 4; i++) {
        if (value & (1 << i)) count++;
    }
    return count;
}

static uint8_t rs485_assignment4_to_ui_mask(uint16_t assignment) {
    assignment &= 0x000F;
    uint8_t mask = 0;
    if (assignment & 0x0008) mask |= (1 << 0);
    if (assignment & 0x0004) mask |= (1 << 1);
    if (assignment & 0x0002) mask |= (1 << 2);
    if (assignment & 0x0001) mask |= (1 << 3);
    return mask;
}

static uint16_t rs485_ui_mask_to_assignment4(uint8_t mask) {
    uint16_t assignment = 0;
    if (mask & (1 << 0)) assignment |= 0x0008;
    if (mask & (1 << 1)) assignment |= 0x0004;
    if (mask & (1 << 2)) assignment |= 0x0002;
    if (mask & (1 << 3)) assignment |= 0x0001;
    return assignment;
}

static uint16_t rs485_capability_from_assignments(uint8_t temp_mask,
                                                  uint8_t lux_count,
                                                  uint8_t co2_count,
                                                  uint8_t presence_count,
                                                  uint8_t relay_count,
                                                  bool projector_enabled,
                                                  bool ac1_enabled,
                                                  bool ac2_enabled) {
    uint16_t capability = 0;
    if (temp_mask != 0) capability |= RS485_CAP_TEMP;
    if (lux_count > 0) capability |= RS485_CAP_LUX;
    if (co2_count > 0) capability |= RS485_CAP_CO2;
    if (presence_count > 0) capability |= RS485_CAP_PRESENCE;
    if (relay_count > 0) capability |= RS485_CAP_LIGHT_RELAY;
    if (projector_enabled) capability |= RS485_CAP_PROJECTOR_IR;
    if (ac1_enabled || ac2_enabled) capability |= RS485_CAP_AC_IR;
    return capability;
}

static bool rs485_counts_are_empty(const uint16_t* registers, uint8_t count) {
    if (!registers) return true;
    for (uint8_t i = 0; i < count; i++) {
        if (registers[i] != 0) return false;
    }
    return true;
}

static void rs485_store_capability(uint8_t address, const uint16_t* registers, uint8_t count) {
    if (!registers || count < RS485_MODBUS_CAPABILITY_ASSIGN_REGS) return;

    data_lock(g_state);
    RS485SlaveState* slave_ptr = nullptr;
    uint8_t index = rs485_find_slave_index_locked(address);
    if (index < RS485_MAX_SLAVES) {
        slave_ptr = &g_state.rs485.slaves[index];
    } else if (address == RS485_MODBUS_PAIRING_ADDR) {
        slave_ptr = &g_state.rs485.pairing_candidate;
        g_state.rs485.pairing_candidate.address = RS485_MODBUS_PAIRING_ADDR;
    }

    if (slave_ptr) {
        RS485SlaveState& slave = *slave_ptr;
        bool unconfigured_counts = rs485_counts_are_empty(registers, RS485_MODBUS_CAPABILITY_ASSIGN_REGS);
        uint8_t temp_mask = rs485_assignment4_to_ui_mask(registers[0]);
        uint8_t lux_count = rs485_popcount4(registers[1]);
        uint8_t co2_count = (uint8_t)(registers[2] > 1 ? 1 : registers[2]);
        uint8_t presence_count = rs485_popcount4(registers[3]);
        uint8_t relay_count = rs485_popcount4(registers[4] & 0x0003);
        bool projector_enabled = registers[5] != 0;
        bool ac1_enabled = registers[6] != 0;
        bool ac2_enabled = registers[7] != 0;

        if (unconfigured_counts) {
            temp_mask = slave.temp_available_mask;
            lux_count = slave.lux_count;
            co2_count = slave.co2_count;
            presence_count = slave.presence_count;
            relay_count = slave.relay_count;
            projector_enabled = (slave.capability & RS485_CAP_PROJECTOR_IR) != 0;
            ac1_enabled = (slave.capability & RS485_CAP_AC_IR) != 0;
            ac2_enabled = slave.ir_count > 2;
        }

        uint8_t available_temp_mask = slave.temp_available_mask;
        if (temp_mask != 0 && available_temp_mask == 0) {
            available_temp_mask = temp_mask;
        }
        if (available_temp_mask == 0 && slave.temp_count > 0) {
            uint8_t fallback_count = slave.temp_count;
            if (fallback_count > DASHBOARD_TEMP_SLOTS) fallback_count = DASHBOARD_TEMP_SLOTS;
            for (uint8_t ch = 0; ch < fallback_count; ch++) available_temp_mask |= (1 << ch);
        }

        slave.temp_count = rs485_popcount4(available_temp_mask);
        slave.temp_available_mask = available_temp_mask;
        slave.co2_count = co2_count;
        slave.presence_count = presence_count;
        slave.relay_count = relay_count;
        slave.ir_count = (projector_enabled ? 1 : 0) + (ac1_enabled ? 1 : 0) + (ac2_enabled ? 1 : 0);
        slave.lux_count = lux_count;
        slave.lcd_count = 0;
        slave.capability = rs485_capability_from_assignments(available_temp_mask,
                                                             slave.lux_count,
                                                             slave.co2_count,
                                                             slave.presence_count,
                                                             slave.relay_count,
                                                             projector_enabled,
                                                             ac1_enabled,
                                                             ac2_enabled);
        slave.capability_synced = true;
        slave.last_capability_ms = millis();
        mapping_manager_update_locked(g_state);
        snprintf(g_state.rs485.status, sizeof(g_state.rs485.status),
                 unconfigured_counts ? "RS485 cap empty 0x%02X" : "RS485 cap 0x%02X 0x%04X",
                 address,
                 slave.capability);
        g_state.ui_needs_update = true;
    }
    if (address == RS485_MODBUS_PAIRING_ADDR &&
        g_state.rs485.pairing_candidate.identity_synced &&
        g_state.rs485.pairing_candidate.capability_synced) {
        g_state.rs485.pairing_candidate_ready = true;
    }
    data_unlock(g_state);
}

static bool rs485_sync_identity_if_needed(uint8_t address) {
    bool should_sync = false;
    data_lock(g_state);
    uint8_t index = rs485_find_slave_index_locked(address);
    if (index < RS485_MAX_SLAVES) {
        RS485SlaveState& slave = g_state.rs485.slaves[index];
        should_sync = !slave.identity_synced ||
                      millis() - slave.last_identity_ms >= RS485_IDENTITY_SYNC_INTERVAL_MS;
    }
    data_unlock(g_state);

    if (!should_sync) return true;

    uint16_t identity[RS485_MODBUS_IDENTITY_REGS] = {0};
    bool ok = rs485_read_holding_registers(address,
                                           RS485_MODBUS_REG_NODE_ADDRESS,
                                           RS485_MODBUS_IDENTITY_REGS,
                                           identity,
                                           "IDENTITY_SYNC");
    if (ok) rs485_store_identity(address, identity, RS485_MODBUS_IDENTITY_REGS);
    return ok;
}

static bool rs485_sync_capability_if_needed(uint8_t address) {
    bool should_sync = false;
    data_lock(g_state);
    uint8_t index = rs485_find_slave_index_locked(address);
    if (index < RS485_MAX_SLAVES) {
        RS485SlaveState& slave = g_state.rs485.slaves[index];
        should_sync = !slave.capability_synced ||
                      millis() - slave.last_capability_ms >= RS485_CAPABILITY_SYNC_INTERVAL_MS;
    }
    data_unlock(g_state);

    if (!should_sync) return true;

    uint16_t capability[RS485_MODBUS_CAPABILITY_ASSIGN_REGS] = {0};
    bool ok = rs485_read_holding_registers(address,
                                           RS485_MODBUS_REG_TEMP_ASSIGNMENT,
                                           RS485_MODBUS_CAPABILITY_ASSIGN_REGS,
                                           capability,
                                           "CAPABILITY_SYNC");
    if (ok) rs485_store_capability(address, capability, RS485_MODBUS_CAPABILITY_ASSIGN_REGS);
    return ok;
}

static void rs485_clear_pairing_candidate_locked() {
    memset(&g_state.rs485.pairing_candidate, 0, sizeof(g_state.rs485.pairing_candidate));
    g_state.rs485.pairing_candidate_ready = false;
    g_state.rs485.pairing_assign_requested = false;
    g_state.rs485.pairing_assign_address = 0;
}

static uint8_t rs485_next_available_address_locked() {
    for (uint16_t address = 2; address < RS485_MODBUS_PAIRING_ADDR; address++) {
        bool used = false;
        uint8_t count = g_state.rs485.slave_count;
        if (count > RS485_MAX_SLAVES) count = RS485_MAX_SLAVES;
        for (uint8_t i = 0; i < count; i++) {
            if (g_state.rs485.slaves[i].address == address) {
                used = true;
                break;
            }
        }
        if (!used) return (uint8_t)address;
    }
    return 0;
}

static uint64_t rs485_mac_from_identity_registers(const uint16_t* identity, uint8_t count) {
    if (!identity || count < RS485_MODBUS_IDENTITY_REGS) return 0;
    uint16_t mac01 = identity[RS485_MODBUS_REG_MAC_0_1 - RS485_MODBUS_REG_NODE_ADDRESS];
    uint16_t mac23 = identity[RS485_MODBUS_REG_MAC_2_3 - RS485_MODBUS_REG_NODE_ADDRESS];
    uint16_t mac45 = identity[RS485_MODBUS_REG_MAC_4_5 - RS485_MODBUS_REG_NODE_ADDRESS];
    return ((uint64_t)(mac01 >> 8) << 40) |
           ((uint64_t)(mac01 & 0xFF) << 32) |
           ((uint64_t)(mac23 >> 8) << 24) |
           ((uint64_t)(mac23 & 0xFF) << 16) |
           ((uint64_t)(mac45 >> 8) << 8) |
           (uint64_t)(mac45 & 0xFF);
}

static bool rs485_saved_address_for_mac(uint64_t mac, uint8_t& address) {
    if (mac == 0) return false;

    bool found = false;
    data_lock(g_state);
    uint8_t count = g_state.rs485.slave_count;
    if (count > RS485_MAX_SLAVES) count = RS485_MAX_SLAVES;
    for (uint8_t i = 0; i < count; i++) {
        const RS485SlaveState& slave = g_state.rs485.slaves[i];
        if (slave.mac == mac &&
            slave.address >= 2 &&
            slave.address < RS485_MODBUS_PAIRING_ADDR) {
            address = slave.address;
            found = true;
            break;
        }
    }
    data_unlock(g_state);
    return found;
}

static void rs485_mark_recovered_slave(uint64_t mac, uint8_t address) {
    data_lock(g_state);
    uint8_t index = rs485_find_slave_index_locked(address);
    if (index < RS485_MAX_SLAVES && g_state.rs485.slaves[index].mac == mac) {
        RS485SlaveState& slave = g_state.rs485.slaves[index];
        slave.online = true;
        slave.degraded = false;
        slave.consecutive_fail = 0;
        slave.last_seen = millis();
        slave.sensor_poll_pending = true;
        if (index < 2) g_state.sensor.slave_online[index] = true;
        snprintf(g_state.rs485.status, sizeof(g_state.rs485.status),
                 "RS485 recovered 0x%02X", address);
        g_state.ui_needs_update = true;
    }
    data_unlock(g_state);
}

static void rs485_apply_saved_assignments_for_address(uint8_t address) {
    uint8_t index = RS485_MAX_SLAVES;
    bool configured = false;

    data_lock(g_state);
    index = rs485_find_slave_index_locked(address);
    if (index < RS485_MAX_SLAVES) {
        const RS485SlaveState& slave = g_state.rs485.slaves[index];
        configured = slave.online &&
                     (slave.profile != DEVICE_PROFILE_UNASSIGNED ||
                      slave.enabled_mask != 0 ||
                      slave.temp_enabled_mask != 0);
    }
    data_unlock(g_state);

    if (configured) {
        rs485_apply_slave_assignments(index);
    }
}

static void rs485_write_recovery_registers_ignore_response(uint16_t* recovery, uint16_t quantity) {
    if (!recovery || quantity != 4) return;

    while (rs485_serial.available()) rs485_serial.read();
    rs485_mark_tx();
    Serial.printf("[RS485] Recovery write TX dst=0x%02X reg=0x%04X qty=%u\n",
                  RS485_MODBUS_PAIRING_ADDR,
                  RS485_MODBUS_REG_RECOVERY_MAC_0_1,
                  quantity);

    uint8_t modbus_code = rs485_modbus.writeHoldingRegister(RS485_MODBUS_PAIRING_ADDR,
                                                            RS485_MODBUS_REG_RECOVERY_MAC_0_1,
                                                            recovery,
                                                            quantity);
    RS485RxResult result = rs485_result_from_modbus_code(modbus_code);
    if (result == RS485_RX_OK) {
        rs485_mark_rx();
    } else {
        Serial.printf("[RS485] Recovery write ignored %s code=%u\n",
                      rs485_rx_result_name(result),
                      modbus_code);
    }
}

static bool rs485_recover_known_pairing_slave(const uint16_t* identity, uint8_t count) {
    uint64_t mac = rs485_mac_from_identity_registers(identity, count);
    uint8_t saved_address = 0;
    if (!rs485_saved_address_for_mac(mac, saved_address)) return false;

    uint16_t recovery[4] = {
        (uint16_t)((((mac >> 40) & 0xFF) << 8) | ((mac >> 32) & 0xFF)),
        (uint16_t)((((mac >> 24) & 0xFF) << 8) | ((mac >> 16) & 0xFF)),
        (uint16_t)((((mac >> 8) & 0xFF) << 8) | (mac & 0xFF)),
        saved_address
    };

    rs485_write_recovery_registers_ignore_response(recovery, 4);

    uint16_t recovered_identity[RS485_MODBUS_IDENTITY_REGS] = {0};
    bool confirmed = rs485_read_holding_registers(saved_address,
                                                  RS485_MODBUS_REG_NODE_ADDRESS,
                                                  RS485_MODBUS_IDENTITY_REGS,
                                                  recovered_identity,
                                                  "RECOVERY_CONFIRM");
    if (!confirmed || rs485_mac_from_identity_registers(recovered_identity, RS485_MODBUS_IDENTITY_REGS) != mac) {
        Serial.printf("[RS485] Recovery confirm failed mac=%012llX addr=0x%02X\n",
                      (unsigned long long)mac,
                      saved_address);
        return false;
    }

    rs485_store_identity(saved_address, recovered_identity, RS485_MODBUS_IDENTITY_REGS);
    rs485_mark_recovered_slave(mac, saved_address);
    rs485_apply_saved_assignments_for_address(saved_address);
    Serial.printf("[RS485] Recovery confirmed mac=%012llX addr=0x%02X\n",
                  (unsigned long long)mac,
                  saved_address);
    return true;
}

static bool rs485_recover_saved_slave(uint64_t mac, uint8_t saved_address, const char* label) {
    if (mac == 0 ||
        saved_address < 2 ||
        saved_address >= RS485_MODBUS_PAIRING_ADDR) {
        return false;
    }

    uint16_t recovery[4] = {
        (uint16_t)((((mac >> 40) & 0xFF) << 8) | ((mac >> 32) & 0xFF)),
        (uint16_t)((((mac >> 24) & 0xFF) << 8) | ((mac >> 16) & 0xFF)),
        (uint16_t)((((mac >> 8) & 0xFF) << 8) | (mac & 0xFF)),
        saved_address
    };

    Serial.printf("[RS485] %s recovery mac=%012llX addr=0x%02X\n",
                  label ? label : "Auto",
                  (unsigned long long)mac,
                  saved_address);
    rs485_write_recovery_registers_ignore_response(recovery, 4);

    uint16_t recovered_identity[RS485_MODBUS_IDENTITY_REGS] = {0};
    bool confirmed = rs485_read_holding_registers(saved_address,
                                                  RS485_MODBUS_REG_NODE_ADDRESS,
                                                  RS485_MODBUS_IDENTITY_REGS,
                                                  recovered_identity,
                                                  label ? label : "AUTO_RECOVERY_CONFIRM");
    if (!confirmed || rs485_mac_from_identity_registers(recovered_identity, RS485_MODBUS_IDENTITY_REGS) != mac) {
        Serial.printf("[RS485] %s recovery confirm failed mac=%012llX addr=0x%02X\n",
                      label ? label : "Auto",
                      (unsigned long long)mac,
                      saved_address);
        return false;
    }

    rs485_store_identity(saved_address, recovered_identity, RS485_MODBUS_IDENTITY_REGS);
    rs485_mark_recovered_slave(mac, saved_address);
    rs485_apply_saved_assignments_for_address(saved_address);
    return true;
}

static void rs485_try_auto_recovery(uint8_t slave_index) {
    uint64_t mac = 0;
    uint8_t address = 0;
    bool should_try = false;
    uint32_t now = millis();

    data_lock(g_state);
    if (!g_state.rs485.pairing_active && slave_index < RS485_MAX_SLAVES) {
        const RS485SlaveState& slave = g_state.rs485.slaves[slave_index];
        bool valid_known_slave = slave.mac != 0 &&
                                 slave.address >= 2 &&
                                 slave.address < RS485_MODBUS_PAIRING_ADDR;
        bool looks_lost = !slave.online ||
                          slave.degraded ||
                          slave.consecutive_fail >= RS485_DEGRADED_THRESHOLD ||
                          (slave.last_seen == 0 || now - slave.last_seen > RS485_OFFLINE_TIMEOUT_MS);
        bool interval_ok = last_auto_recovery_ms[slave_index] == 0 ||
                           now - last_auto_recovery_ms[slave_index] >= RS485_AUTO_RECOVERY_INTERVAL_MS;
        should_try = valid_known_slave && looks_lost && interval_ok;
        if (should_try) {
            mac = slave.mac;
            address = slave.address;
            last_auto_recovery_ms[slave_index] = now;
            snprintf(g_state.rs485.status, sizeof(g_state.rs485.status),
                     "RS485 recovery 0x%02X", address);
            g_state.ui_needs_update = true;
        }
    }
    data_unlock(g_state);

    if (!should_try) return;

    bool ok = rs485_recover_saved_slave(mac, address, "AUTO_RECOVERY");
    data_lock(g_state);
    if (!ok) {
        snprintf(g_state.rs485.status, sizeof(g_state.rs485.status),
                 "RS485 recovery failed 0x%02X", address);
        g_state.ui_needs_update = true;
    }
    data_unlock(g_state);
}

static void rs485_store_assigned_candidate(uint8_t new_address) {
    data_lock(g_state);
    if (!g_state.rs485.pairing_candidate_ready || new_address == 0) {
        data_unlock(g_state);
        return;
    }

    uint8_t index = rs485_find_slave_index_locked(new_address);
    if (index >= RS485_MAX_SLAVES) {
        uint8_t count = g_state.rs485.slave_count;
        if (count > RS485_MAX_SLAVES) count = RS485_MAX_SLAVES;
        for (uint8_t i = 0; i < count; i++) {
            if (g_state.rs485.slaves[i].address == 0 && g_state.rs485.slaves[i].uid == 0) {
                index = i;
                break;
            }
        }
        if (index >= RS485_MAX_SLAVES && g_state.rs485.slave_count < RS485_MAX_SLAVES) {
            index = g_state.rs485.slave_count++;
        } else if (index >= RS485_MAX_SLAVES) {
            data_unlock(g_state);
            return;
        }
    }

    RS485SlaveState candidate = g_state.rs485.pairing_candidate;
    candidate.address = new_address;
    candidate.online = true;
    candidate.degraded = false;
    candidate.last_seen = millis();
    candidate.consecutive_fail = 0;
    g_state.rs485.slaves[index] = candidate;
    if (index < 2) g_state.sensor.slave_online[index] = true;
    mapping_manager_update_locked(g_state);

    g_state.rs485.pairing_active = false;
    g_state.rs485.poll_enabled = pairing_restore_poll_enabled;
    rs485_clear_pairing_candidate_locked();
    snprintf(g_state.rs485.status, sizeof(g_state.rs485.status),
             "RS485 paired 0x%02X", new_address);
    g_state.ui_needs_update = true;
    data_unlock(g_state);

    data_save_rs485_config(g_state);
}

static bool rs485_pairing_candidate_assignments(uint16_t assignments[RS485_MODBUS_CAPABILITY_ASSIGN_REGS]) {
    if (!assignments) return false;

    data_lock(g_state);
    bool ready = g_state.rs485.pairing_candidate_ready;
    RS485SlaveState candidate = g_state.rs485.pairing_candidate;
    data_unlock(g_state);

    if (!ready) return false;
    assignments[0] = rs485_ui_mask_to_assignment4(candidate.temp_available_mask);
    assignments[1] = candidate.lux_count > 0 ? 0x000F : 0;
    assignments[2] = candidate.co2_count > 0 ? 1 : 0;
    assignments[3] = candidate.presence_count > 0 ? 0x000F : 0;
    assignments[4] = candidate.relay_count >= 2 ? 0x0003 : (candidate.relay_count == 1 ? 0x0002 : 0);
    assignments[5] = (candidate.capability & RS485_CAP_PROJECTOR_IR) ? 1 : 0;
    assignments[6] = (candidate.capability & RS485_CAP_AC_IR) ? 1 : 0;
    assignments[7] = candidate.ir_count > 2 ? 1 : 0;
    return true;
}

bool rs485_apply_slave_assignments(uint8_t slave_index) {
    RS485SlaveState slave = {};

    data_lock(g_state);
    if (slave_index < RS485_MAX_SLAVES) {
        slave = g_state.rs485.slaves[slave_index];
    }
    data_unlock(g_state);

    if (slave_index >= RS485_MAX_SLAVES || slave.address == 0 || !slave.online) {
        return false;
    }

    uint16_t assignments[RS485_MODBUS_CAPABILITY_ASSIGN_REGS] = {0};
    assignments[0] = (slave.enabled_mask & RS485_CAP_TEMP)
                         ? rs485_ui_mask_to_assignment4(slave.temp_enabled_mask & 0x0F)
                         : 0;
    assignments[1] = (slave.enabled_mask & RS485_CAP_LUX) ? 0x000F : 0;
    assignments[2] = (slave.enabled_mask & RS485_CAP_CO2) ? (slave.co2_count == 0 ? 1 : slave.co2_count) : 0;
    assignments[3] = (slave.enabled_mask & RS485_CAP_PRESENCE) ? 0x000F : 0;
    assignments[4] = (slave.enabled_mask & RS485_CAP_LIGHT_RELAY)
                         ? (slave.relay_count >= 2 || slave.relay_count == 0 ? 0x0003 : 0x0002)
                         : 0;
    assignments[5] = (slave.enabled_mask & RS485_CAP_PROJECTOR_IR) ? 1 : 0;
    assignments[6] = (slave.enabled_mask & RS485_CAP_AC_IR) ? 1 : 0;
    assignments[7] = (slave.enabled_mask & RS485_CAP_AC_IR) && slave.ir_count > 2 ? 1 : 0;

    Serial.printf("[RS485] Apply assignments addr=0x%02X temp=0x%04X lux=0x%04X co2=%u presence=0x%04X relay=0x%04X proj=%u ac1=%u ac2=%u\n",
                  slave.address,
                  assignments[0],
                  assignments[1],
                  assignments[2],
                  assignments[3],
                  assignments[4],
                  assignments[5],
                  assignments[6],
                  assignments[7]);

    bool ok = rs485_write_holding_registers(slave.address,
                                            RS485_MODBUS_REG_TEMP_ASSIGNMENT,
                                            assignments,
                                            RS485_MODBUS_CAPABILITY_ASSIGN_REGS,
                                            "APPLY_ASSIGNMENTS");
    return ok;
}

static bool rs485_find_control_slave_locked(uint16_t capability,
                                            DashboardLogicalId logical_id,
                                            RS485SlaveState& out) {
    const LogicalMapping& mapping = g_state.rs485.mappings[logical_id];
    uint8_t count = g_state.rs485.slave_count;
    if (count > RS485_MAX_SLAVES) count = RS485_MAX_SLAVES;

    if (mapping.assigned) {
        for (uint8_t i = 0; i < count; i++) {
            const RS485SlaveState& slave = g_state.rs485.slaves[i];
            bool uid_match = mapping.slave_uid != 0 && slave.uid == mapping.slave_uid;
            bool addr_match = mapping.slave_uid == 0 && mapping.slave_addr != 0 &&
                              slave.address == mapping.slave_addr;
            if ((uid_match || addr_match) && slave.online &&
                (slave.enabled_mask & capability)) {
                out = slave;
                return true;
            }
        }
    }

    for (uint8_t i = 0; i < count; i++) {
        const RS485SlaveState& slave = g_state.rs485.slaves[i];
        if (slave.online && slave.address != 0 && (slave.enabled_mask & capability)) {
            out = slave;
            return true;
        }
    }
    return false;
}

static bool rs485_find_control_slave(uint16_t capability,
                                     DashboardLogicalId logical_id,
                                     RS485SlaveState& out) {
    bool found = false;
    data_lock(g_state);
    found = rs485_find_control_slave_locked(capability, logical_id, out);
    data_unlock(g_state);
    return found;
}

static bool rs485_find_enabled_slave(uint16_t capability, RS485SlaveState& out) {
    bool found = false;
    data_lock(g_state);
    uint8_t count = g_state.rs485.slave_count;
    if (count > RS485_MAX_SLAVES) count = RS485_MAX_SLAVES;
    for (uint8_t i = 0; i < count; i++) {
        const RS485SlaveState& slave = g_state.rs485.slaves[i];
        if (slave.online && slave.address != 0 && (slave.enabled_mask & capability)) {
            out = slave;
            found = true;
            break;
        }
    }
    data_unlock(g_state);
    return found;
}

static bool rs485_light_readback_matches(uint8_t address, uint8_t channel, bool expected_on) {
    bool matches = false;
    data_lock(g_state);
    uint8_t index = rs485_find_slave_index_locked(address);
    if (index < RS485_MAX_SLAVES) {
        const RS485SlaveState& slave = g_state.rs485.slaves[index];
        uint8_t quantity = slave.relay_count == 0 ? 1 : slave.relay_count;
        if (quantity > 2) quantity = 2;
        if (channel >= 1 && channel <= quantity) {
            matches = (slave.relay_state[channel - 1] != 0) == expected_on;
        } else if (channel == 0) {
            matches = true;
            for (uint8_t relay = 0; relay < quantity; relay++) {
                if ((slave.relay_state[relay] != 0) != expected_on) {
                    matches = false;
                    break;
                }
            }
        }
    }
    data_unlock(g_state);
    return matches;
}

static bool rs485_write_light_command(uint8_t channel, bool on) {
    RS485SlaveState slave = {};
    if (!rs485_find_enabled_slave(RS485_CAP_LIGHT_RELAY, slave)) {
        data_lock(g_state);
        strncpy(g_state.rs485.status, "No relay slave mapped", sizeof(g_state.rs485.status) - 1);
        g_state.rs485.status[sizeof(g_state.rs485.status) - 1] = '\0';
        g_state.ui_needs_update = true;
        data_unlock(g_state);
        return false;
    }

    uint8_t quantity = slave.relay_count == 0 ? 1 : slave.relay_count;
    if (quantity > 2) quantity = 2;
    if (channel > quantity) {
        data_lock(g_state);
        snprintf(g_state.rs485.status, sizeof(g_state.rs485.status),
                 "Relay channel %u unavailable", channel);
        g_state.ui_needs_update = true;
        data_unlock(g_state);
        return false;
    }

    if (channel >= 1 && channel <= 2) {
        uint16_t reg = channel == 1 ? RS485_MODBUS_REG_RELAY_1 : RS485_MODBUS_REG_RELAY_2;
        bool ok = rs485_write_holding_register(slave.address, reg, on ? 1 : 0, "LIGHT_CHANNEL_COMMAND");
        if (!ok || !rs485_poll_sensor_registers(slave.address)) return false;
        return rs485_light_readback_matches(slave.address, channel, on);
    }

    uint16_t relays[2] = {
        static_cast<uint16_t>(on ? 1 : 0),
        static_cast<uint16_t>(on ? 1 : 0)
    };

    bool ok = quantity > 1
                  ? rs485_write_holding_registers(slave.address, RS485_MODBUS_REG_RELAY_1,
                                                  relays, quantity, "LIGHT_COMMAND")
                  : rs485_write_holding_register(slave.address, RS485_MODBUS_REG_RELAY_1,
                                                 relays[0], "LIGHT_COMMAND");
    if (!ok || !rs485_poll_sensor_registers(slave.address)) return false;
    return rs485_light_readback_matches(slave.address, 0, on);
}

static uint8_t rs485_normalize_ac_enum(uint8_t value) {
    return value <= 99 ? value : 99;
}

static bool rs485_write_ac_command(bool power, float target_c, uint8_t mode, uint8_t fan_speed, uint8_t swing_mode) {
    RS485SlaveState slave = {};
    if (!rs485_find_control_slave(RS485_CAP_AC_IR, LOGICAL_AC_CONTROL, slave)) {
        data_lock(g_state);
        strncpy(g_state.rs485.status, "No AC slave mapped", sizeof(g_state.rs485.status) - 1);
        g_state.rs485.status[sizeof(g_state.rs485.status) - 1] = '\0';
        g_state.ui_needs_update = true;
        data_unlock(g_state);
        return false;
    }

    if (target_c < 16.0f) target_c = 16.0f;
    if (target_c > 30.0f) target_c = 30.0f;
    uint16_t temp = (uint16_t)(target_c + 0.5f) * 10;
    fan_speed = rs485_normalize_ac_enum(fan_speed);
    swing_mode = rs485_normalize_ac_enum(swing_mode);

    bool ok = rs485_write_holding_register(slave.address, RS485_MODBUS_REG_AC_1_POWER,
                                           power ? 1 : 0, "AC1_POWER");
    ok = rs485_write_holding_register(slave.address, RS485_MODBUS_REG_AC_1_SET_TEMP,
                                      temp, "AC1_TEMP") && ok;
    ok = rs485_write_holding_register(slave.address, RS485_MODBUS_REG_AC_1_MODE,
                                      mode, "AC1_MODE") && ok;
    ok = rs485_write_holding_register(slave.address, RS485_MODBUS_REG_AC_1_FAN_SPEED,
                                      fan_speed, "AC1_FAN") && ok;
    ok = rs485_write_holding_register(slave.address, RS485_MODBUS_REG_AC_1_SWING_VERTICAL,
                                      swing_mode, "AC1_SWING_V") && ok;
    ok = rs485_write_holding_register(slave.address, RS485_MODBUS_REG_AC_1_SWING_HORIZONTAL,
                                      99, "AC1_SWING_H") && ok;

    bool mirror_ac2 = slave.profile == IR_COMBO_NODE || slave.ir_count > 2;
    if (mirror_ac2) {
        ok = rs485_write_holding_register(slave.address, RS485_MODBUS_REG_AC_2_POWER,
                                          power ? 1 : 0, "AC2_POWER") && ok;
        ok = rs485_write_holding_register(slave.address, RS485_MODBUS_REG_AC_2_SET_TEMP,
                                          temp, "AC2_TEMP") && ok;
        ok = rs485_write_holding_register(slave.address, RS485_MODBUS_REG_AC_2_MODE,
                                          mode, "AC2_MODE") && ok;
        ok = rs485_write_holding_register(slave.address, RS485_MODBUS_REG_AC_2_FAN_SPEED,
                                          fan_speed, "AC2_FAN") && ok;
        ok = rs485_write_holding_register(slave.address, RS485_MODBUS_REG_AC_2_SWING_VERTICAL,
                                          swing_mode, "AC2_SWING_V") && ok;
        ok = rs485_write_holding_register(slave.address, RS485_MODBUS_REG_AC_2_SWING_HORIZONTAL,
                                          99, "AC2_SWING_H") && ok;
    }
    return ok;
}

static bool rs485_write_projector_command(bool power, uint8_t input) {
    RS485SlaveState slave = {};
    if (!rs485_find_control_slave(RS485_CAP_PROJECTOR_IR, LOGICAL_PROJECTOR_CONTROL, slave)) {
        data_lock(g_state);
        strncpy(g_state.rs485.status, "No projector slave mapped", sizeof(g_state.rs485.status) - 1);
        g_state.rs485.status[sizeof(g_state.rs485.status) - 1] = '\0';
        g_state.ui_needs_update = true;
        data_unlock(g_state);
        return false;
    }

    bool ok = rs485_write_holding_register(slave.address, RS485_MODBUS_REG_PROJECTOR_POWER,
                                           power ? 1 : 0, "PROJECTOR_POWER");
    if (input != 0) {
        ok = rs485_write_holding_register(slave.address, RS485_MODBUS_REG_PROJECTOR_INPUT,
                                          input, "PROJECTOR_INPUT") && ok;
    }
    return ok;
}

static bool rs485_scan_pairing_candidate() {
    uint16_t identity[RS485_MODBUS_IDENTITY_REGS] = {0};
    uint16_t capability[RS485_MODBUS_CAPABILITY_ASSIGN_REGS] = {0};

    bool identity_ok = rs485_read_holding_registers(RS485_MODBUS_PAIRING_ADDR,
                                                    RS485_MODBUS_REG_NODE_ADDRESS,
                                                    RS485_MODBUS_IDENTITY_REGS,
                                                    identity,
                                                    "PAIR_IDENTITY");
    if (!identity_ok) return false;

    if (rs485_recover_known_pairing_slave(identity, RS485_MODBUS_IDENTITY_REGS)) {
        data_lock(g_state);
        g_state.rs485.pairing_active = false;
        g_state.rs485.poll_enabled = pairing_restore_poll_enabled;
        rs485_clear_pairing_candidate_locked();
        g_state.ui_needs_update = true;
        data_unlock(g_state);
        return true;
    }

    bool capability_ok = rs485_read_holding_registers(RS485_MODBUS_PAIRING_ADDR,
                                                      RS485_MODBUS_REG_TEMP_ASSIGNMENT,
                                                      RS485_MODBUS_CAPABILITY_ASSIGN_REGS,
                                                      capability,
                                                      "PAIR_CAPABILITY");
    if (!capability_ok) return false;

    rs485_store_identity(RS485_MODBUS_PAIRING_ADDR, identity, RS485_MODBUS_IDENTITY_REGS);
    rs485_store_capability(RS485_MODBUS_PAIRING_ADDR, capability, RS485_MODBUS_CAPABILITY_ASSIGN_REGS);

    data_lock(g_state);
    snprintf(g_state.rs485.status, sizeof(g_state.rs485.status),
             "Candidate UID %08lX",
             (unsigned long)g_state.rs485.pairing_candidate.uid);
    g_state.ui_needs_update = true;
    data_unlock(g_state);

    Serial.printf("[RS485] Pairing candidate uid=0x%08lX cap=0x%04X\n",
                  (unsigned long)g_state.rs485.pairing_candidate.uid,
                  g_state.rs485.pairing_candidate.capability);
    return true;
}

static bool rs485_scan_known_slave_during_pairing() {
    uint8_t index = RS485_MAX_SLAVES;
    uint8_t address = 0;
    uint64_t expected_mac = 0;

    data_lock(g_state);
    uint8_t count = g_state.rs485.slave_count;
    if (count > RS485_MAX_SLAVES) count = RS485_MAX_SLAVES;
    for (uint8_t checked = 0; checked < count; checked++) {
        uint8_t candidate_index = pairing_known_scan_index;
        pairing_known_scan_index = (pairing_known_scan_index + 1) % (count == 0 ? 1 : count);
        const RS485SlaveState& slave = g_state.rs485.slaves[candidate_index];
        if (slave.uid == RS485_DUMMY_UI_UID) continue;
        if (slave.mac == 0) continue;
        if (slave.address < 2 || slave.address >= RS485_MODBUS_PAIRING_ADDR) continue;
        index = candidate_index;
        address = slave.address;
        expected_mac = slave.mac;
        break;
    }
    data_unlock(g_state);

    if (index >= RS485_MAX_SLAVES || address == 0 || expected_mac == 0) return false;

    uint16_t identity[RS485_MODBUS_IDENTITY_REGS] = {0};
    bool identity_ok = rs485_read_holding_registers(address,
                                                    RS485_MODBUS_REG_NODE_ADDRESS,
                                                    RS485_MODBUS_IDENTITY_REGS,
                                                    identity,
                                                    "PAIR_KNOWN_IDENTITY");
    if (!identity_ok) return false;

    uint64_t actual_mac = rs485_mac_from_identity_registers(identity, RS485_MODBUS_IDENTITY_REGS);
    if (actual_mac != expected_mac) {
        Serial.printf("[RS485] Pair known MAC mismatch addr=0x%02X expected=%012llX actual=%012llX\n",
                      address,
                      (unsigned long long)expected_mac,
                      (unsigned long long)actual_mac);
        return false;
    }

    rs485_store_identity(address, identity, RS485_MODBUS_IDENTITY_REGS);
    rs485_sync_capability_if_needed(address);

    data_lock(g_state);
    snprintf(g_state.rs485.status, sizeof(g_state.rs485.status),
             "RS485 known alive 0x%02X", address);
    g_state.ui_needs_update = true;
    data_unlock(g_state);

    Serial.printf("[RS485] Pairing confirmed known slave addr=0x%02X mac=%012llX\n",
                  address,
                  (unsigned long long)actual_mac);
    rs485_apply_saved_assignments_for_address(address);
    return true;
}

static bool rs485_assign_pairing_candidate(uint8_t requested_address) {
    uint8_t new_address = requested_address;
    data_lock(g_state);
    bool ready = g_state.rs485.pairing_candidate_ready;
    if (new_address == 0) new_address = rs485_next_available_address_locked();
    data_unlock(g_state);

    if (!ready || new_address == 0 || new_address >= RS485_MODBUS_PAIRING_ADDR) {
        data_lock(g_state);
        strncpy(g_state.rs485.status, "Pair assign unavailable", sizeof(g_state.rs485.status) - 1);
        g_state.rs485.status[sizeof(g_state.rs485.status) - 1] = '\0';
        g_state.ui_needs_update = true;
        data_unlock(g_state);
        return false;
    }

    uint16_t assignments[RS485_MODBUS_CAPABILITY_ASSIGN_REGS] = {0};
    if (!rs485_pairing_candidate_assignments(assignments)) return false;

    bool counts_ok = rs485_write_holding_registers(RS485_MODBUS_PAIRING_ADDR,
                                                   RS485_MODBUS_REG_TEMP_ASSIGNMENT,
                                                   assignments,
                                                   RS485_MODBUS_CAPABILITY_ASSIGN_REGS,
                                                   "PAIR_SET_ASSIGN");
    if (!counts_ok) return false;

    bool address_ok = rs485_write_holding_register(RS485_MODBUS_PAIRING_ADDR,
                                                   RS485_MODBUS_REG_NODE_ADDRESS,
                                                   new_address,
                                                   "PAIR_SET_ADDRESS");
    if (!address_ok) return false;

    rs485_store_assigned_candidate(new_address);
    Serial.printf("[RS485] Pairing assigned new_addr=0x%02X\n", new_address);
    return true;
}

static void rs485_seed_fake_pairing_candidate() {
    uint16_t identity[RS485_MODBUS_IDENTITY_REGS] = {0};
    uint16_t capability[RS485_MODBUS_CAPABILITY_ASSIGN_REGS] = {0};
    identity[0] = RS485_MODBUS_PAIRING_ADDR;
    identity[1] = 210;
    identity[2] = 0x1122;
    identity[3] = 0x3344;
    identity[4] = 0x5566;

    capability[0] = 0x000F; // TEMP_SENSOR_ASSIGNMENT: Temp 1-4
    capability[1] = 0x000F; // LUX_SENSOR_ASSIGNMENT: Lux 1-4
    capability[2] = 1;      // CO2_SENSOR_COUNT
    capability[3] = 0x000F; // PRESENCE_SENSOR_ASSIGNMENT: Presence 1-4
    capability[4] = 0x0003; // RELAY_ASSIGNMENT: Relay 1-2
    capability[5] = 1;      // IR_PROJECTOR_ENABLE
    capability[6] = 1;      // IR_AC_1_ENABLE
    capability[7] = 1;      // IR_AC_2_ENABLE

    rs485_store_identity(RS485_MODBUS_PAIRING_ADDR, identity, RS485_MODBUS_IDENTITY_REGS);
    rs485_store_capability(RS485_MODBUS_PAIRING_ADDR, capability, RS485_MODBUS_CAPABILITY_ASSIGN_REGS);

    data_lock(g_state);
    g_state.rs485.pairing_active = true;
    g_state.rs485.pairing_started_ms = millis();
    if (g_state.rs485.pairing_timeout_ms == 0) {
        g_state.rs485.pairing_timeout_ms = RS485_PAIRING_TIMEOUT_MS;
    }
    g_state.rs485.poll_enabled = false;
    snprintf(g_state.rs485.status, sizeof(g_state.rs485.status),
             "Fake candidate UID %08lX",
             (unsigned long)g_state.rs485.pairing_candidate.uid);
    g_state.ui_needs_update = true;
    data_unlock(g_state);
}

static bool rs485_temp_value_valid(uint16_t raw) {
    return raw != RS485_MODBUS_TEMP_ERROR_SENTINEL &&
           raw != RS485_MODBUS_TEMP_UNASSIGNED;
}

static bool rs485_temp_channel_assigned(uint16_t raw) {
    return raw != RS485_MODBUS_TEMP_UNASSIGNED;
}

static bool rs485_u16_value_valid(uint16_t raw) {
    return raw != RS485_MODBUS_U16_ERROR_SENTINEL &&
           raw != RS485_MODBUS_U16_UNASSIGNED;
}

static void rs485_update_sensors_from_block(uint8_t address,
                                            uint16_t capability,
                                            const uint16_t* block,
                                            uint8_t block_count) {
    if (!block) return;

    data_lock(g_state);
    uint8_t slave_index = rs485_find_slave_index_locked(address);
    if (slave_index < RS485_MAX_SLAVES) {
        g_state.rs485.slaves[slave_index].last_seen = millis();
        g_state.rs485.slaves[slave_index].online = true;
        g_state.rs485.slaves[slave_index].sensor_poll_pending = false;
        if (slave_index < 2) g_state.sensor.slave_online[slave_index] = true;
    }

    if (capability & RS485_CAP_TEMP) {
        uint8_t available_mask = 0;
        for (uint8_t i = 0; i < DASHBOARD_TEMP_SLOTS; i++) {
            if (i >= block_count) break;
            if (rs485_temp_channel_assigned(block[i])) available_mask |= (1 << i);
        }
        if (slave_index < RS485_MAX_SLAVES && available_mask != 0) {
            g_state.rs485.slaves[slave_index].temp_available_mask |= available_mask;
            g_state.rs485.slaves[slave_index].temp_count = rs485_popcount4(g_state.rs485.slaves[slave_index].temp_available_mask);
            g_state.rs485.slaves[slave_index].capability |= RS485_CAP_TEMP;
        }

        uint8_t temp_mask = slave_index < RS485_MAX_SLAVES ? g_state.rs485.slaves[slave_index].temp_enabled_mask : 0x0F;
        for (uint8_t i = 0; i < DASHBOARD_TEMP_SLOTS; i++) {
            if (i >= block_count) break;
            uint16_t raw = block[i];
            if ((temp_mask & (1 << i)) && rs485_temp_value_valid(raw)) {
                float value = ((int16_t)raw) / 10.0f;
                if (slave_index < RS485_MAX_SLAVES) {
                    g_state.rs485.slaves[slave_index].temp[i] = value;
                    g_state.rs485.slaves[slave_index].temp_valid[i] = true;
                }
                g_state.sensor.temp[i] = value;
                g_state.sensor.sensor_error[i] = false;
            } else {
                if (slave_index < RS485_MAX_SLAVES) {
                    g_state.rs485.slaves[slave_index].temp_valid[i] = false;
                }
            }
        }
    }

    uint16_t co2 = block_count > 8 ? block[8] : RS485_MODBUS_U16_UNASSIGNED;
    if ((capability & RS485_CAP_CO2) && rs485_u16_value_valid(co2)) {
        if (slave_index < RS485_MAX_SLAVES) {
            g_state.rs485.slaves[slave_index].co2 = co2;
            g_state.rs485.slaves[slave_index].co2_valid = true;
        }
        g_state.sensor.co2 = co2;
    }

    if (capability & RS485_CAP_LUX) {
        uint32_t sum = 0;
        uint8_t valid = 0;
        float lux_channels[4] = {0, 0, 0, 0};
        bool lux_channel_valid[4] = {false, false, false, false};
        for (uint8_t i = 0; i < 4; i++) {
            uint16_t raw = block_count > (4 + i) ? block[4 + i] : RS485_MODBUS_U16_UNASSIGNED;
            if (!rs485_u16_value_valid(raw)) continue;
            lux_channels[i] = (float)raw;
            lux_channel_valid[i] = true;
            sum += raw;
            valid++;
        }
        if (valid > 0) {
            float lux = (float)sum / valid;
            if (slave_index < RS485_MAX_SLAVES) {
                g_state.rs485.slaves[slave_index].lux = lux;
                g_state.rs485.slaves[slave_index].lux_valid = true;
                memcpy(g_state.rs485.slaves[slave_index].lux_channel, lux_channels, sizeof(lux_channels));
                memcpy(g_state.rs485.slaves[slave_index].lux_channel_valid, lux_channel_valid, sizeof(lux_channel_valid));
            }
            g_state.sensor.lux = lux;
        } else if (slave_index < RS485_MAX_SLAVES) {
            g_state.rs485.slaves[slave_index].lux_valid = false;
            memset(g_state.rs485.slaves[slave_index].lux_channel_valid, 0, sizeof(g_state.rs485.slaves[slave_index].lux_channel_valid));
        }
    }

    if (capability & RS485_CAP_PRESENCE) {
        bool any_valid = false;
        bool present = false;
        for (uint8_t i = 0; i < 4; i++) {
            uint16_t raw = block_count > (9 + i) ? block[9 + i] : RS485_MODBUS_U16_UNASSIGNED;
            if (!rs485_u16_value_valid(raw)) continue;
            any_valid = true;
            if (raw != 0) present = true;
        }
        if (slave_index < RS485_MAX_SLAVES) {
            g_state.rs485.slaves[slave_index].human_presence = present;
            g_state.rs485.slaves[slave_index].human_presence_valid = any_valid;
        }
        if (any_valid) g_state.sensor.human_presence = present;
    }

    if (capability & RS485_CAP_LIGHT_RELAY) {
        uint16_t raw_relay1 = block_count > 13 ? block[13] : RS485_MODBUS_U16_UNASSIGNED;
        uint16_t raw_relay2 = block_count > 14 ? block[14] : RS485_MODBUS_U16_UNASSIGNED;
        bool relay1 = rs485_u16_value_valid(raw_relay1) && raw_relay1 != 0;
        bool relay2 = rs485_u16_value_valid(raw_relay2) && raw_relay2 != 0;
        if (slave_index < RS485_MAX_SLAVES) {
            g_state.rs485.slaves[slave_index].relay_state[0] = relay1 ? 1 : 0;
            g_state.rs485.slaves[slave_index].relay_state[1] = relay2 ? 1 : 0;
        }
        g_state.sensor.light_on = relay1 || relay2;
    }
    mapping_manager_update_locked(g_state);
    g_state.last_data_ts = millis();
    g_state.ui_needs_update = true;
    snprintf(g_state.rs485.status, sizeof(g_state.rs485.status), "RS485 sensors 0x%02X", address);
    data_unlock(g_state);
}

static bool rs485_plan_for_command(uint8_t cmd,
                                   const uint8_t* payload,
                                   uint8_t len,
                                   ModbusRequestPlan& plan) {
    plan.write = false;
    plan.function_code = RS485_MODBUS_FC_READ_HOLDING;
    plan.reg = RS485_MODBUS_REG_NODE_ADDRESS;
    plan.quantity = 1;
    plan.value = 0;
    plan.label = "NODE_ADDRESS";

    switch (cmd) {
        case RS485_CMD_PING:
            plan.reg = RS485_MODBUS_REG_NODE_ADDRESS;
            plan.quantity = 1;
            plan.label = "PING_ADDRESS";
            return true;

        case RS485_CMD_GET_INFO:
            plan.reg = RS485_MODBUS_REG_NODE_ADDRESS;
            plan.quantity = RS485_MODBUS_IDENTITY_REGS;
            plan.label = "IDENTITY";
            return true;

        case RS485_CMD_READ_SENSOR:
            plan.reg = RS485_MODBUS_REG_TEMP_1_X10;
            plan.quantity = RS485_MODBUS_SENSOR_BLOCK_REGS;
            plan.label = "SENSOR_BLOCK";
            return true;

        case RS485_CMD_GET_CONFIG:
            plan.reg = RS485_MODBUS_REG_TEMP_ASSIGNMENT;
            plan.quantity = RS485_MODBUS_CAPABILITY_ASSIGN_REGS;
            plan.label = "CAPABILITY_ASSIGN";
            return true;

        case RS485_CMD_SET_OUTPUT:
            plan.write = true;
            plan.function_code = RS485_MODBUS_FC_WRITE_SINGLE;
            plan.reg = RS485_MODBUS_REG_RELAY_1;
            plan.quantity = 1;
            plan.value = rs485_payload_u16(payload, len, 0);
            plan.label = "RELAY_1";
            return true;

        default:
            return false;
    }
}

static void rs485_fill_compat_response(uint8_t dst,
                                       uint8_t cmd,
                                       uint8_t seq,
                                       const uint16_t* registers,
                                       uint16_t quantity,
                                       RS485Frame* response) {
    if (!response) return;

    memset(response, 0, sizeof(*response));
    response->dst = RS485_MASTER_ADDR;
    response->src = dst;
    response->type = RS485_TYPE_RESPONSE;
    response->cmd = cmd;
    response->seq = seq;
    response->len = (quantity * 2 > RS485_MAX_PAYLOAD) ? RS485_MAX_PAYLOAD : quantity * 2;

    for (uint8_t i = 0; i < response->len / 2; i++) {
        response->payload[i * 2] = (registers[i] >> 8) & 0xFF;
        response->payload[i * 2 + 1] = registers[i] & 0xFF;
    }
}

static void rs485_update_sensor_from_modbus_payload(uint8_t address, const uint8_t* payload, uint8_t len) {
    if (!payload || len < 2) return;

    if (len >= RS485_MODBUS_SENSOR_BLOCK_REGS * 2) {
        uint16_t block[RS485_MODBUS_SENSOR_BLOCK_REGS] = {0};
        for (uint8_t i = 0; i < RS485_MODBUS_SENSOR_BLOCK_REGS; i++) {
            block[i] = ((uint16_t)payload[i * 2] << 8) | payload[i * 2 + 1];
        }

        uint16_t capability = RS485_CAP_TEMP;
        data_lock(g_state);
        uint8_t index = rs485_find_slave_index_locked(address);
        if (index < RS485_MAX_SLAVES && g_state.rs485.slaves[index].capability != 0) {
            capability = g_state.rs485.slaves[index].capability;
        }
        data_unlock(g_state);

        rs485_update_sensors_from_block(address, capability, block, RS485_MODBUS_SENSOR_BLOCK_REGS);
        return;
    }

    int16_t temp_x10 = (int16_t)(((uint16_t)payload[0] << 8) | payload[1]);

    data_lock(g_state);
    uint8_t index = rs485_find_slave_index_locked(address);
    if (index < RS485_MAX_SLAVES) {
        g_state.rs485.slaves[index].last_seen = millis();
        g_state.rs485.slaves[index].online = true;
        if (index < 2) g_state.sensor.slave_online[index] = true;
    }
    g_state.sensor.temp[0] = temp_x10 / 10.0f;
    g_state.sensor.sensor_error[0] = false;
    g_state.last_data_ts = millis();
    g_state.ui_needs_update = true;
    snprintf(g_state.rs485.status, sizeof(g_state.rs485.status), "RS485 temp 0x%02X", address);
    data_unlock(g_state);
}

static bool rs485_poll_sensor_registers(uint8_t address) {
    uint16_t capability = 0;
    bool capability_synced = false;

    data_lock(g_state);
    uint8_t index = rs485_find_slave_index_locked(address);
    if (index < RS485_MAX_SLAVES) {
        RS485SlaveState& slave = g_state.rs485.slaves[index];
        capability_synced = slave.capability_synced;
        capability = slave.capability;
    }
    data_unlock(g_state);

    if (!capability_synced || capability == 0) capability = RS485_CAP_TEMP;

    uint16_t sensor_block[RS485_MODBUS_SENSOR_BLOCK_REGS] = {0};
    if (rs485_read_holding_registers(address,
                                     RS485_MODBUS_REG_TEMP_1_X10,
                                     RS485_MODBUS_SENSOR_BLOCK_REGS,
                                     sensor_block,
                                     "SENSOR_BLOCK_POLL")) {
        rs485_update_sensors_from_block(address,
                                        capability,
                                        sensor_block,
                                        RS485_MODBUS_SENSOR_BLOCK_REGS);
        return true;
    }
    return false;
}

static void rs485_prepare_for_debug_force(uint8_t dst,
                                          uint8_t cmd,
                                          uint8_t seq,
                                          RS485TransactionResult& result,
                                          RS485Frame* response) {
    debug_force_rx_result = false;
    result.rx_result = debug_forced_rx_result;
    result.response_type = (debug_forced_rx_result == RS485_RX_OK) ? RS485_TYPE_RESPONSE : RS485_TYPE_ERROR;

    if (debug_forced_rx_result == RS485_RX_OK) {
        uint16_t fake_registers[9] = {0};
        uint16_t quantity = 1;

        if (cmd == RS485_CMD_GET_INFO) {
            fake_registers[0] = dst;
            fake_registers[1] = 210;
            fake_registers[2] = 0xCAFE;
            fake_registers[3] = 0x0000;
            fake_registers[4] = dst;
            quantity = RS485_MODBUS_IDENTITY_REGS;
            rs485_store_identity(dst, fake_registers, quantity);
        } else if (cmd == RS485_CMD_GET_CONFIG) {
            fake_registers[0] = 0x000F;
            fake_registers[1] = 0x000F;
            fake_registers[2] = 1;
            fake_registers[3] = 0x000F;
            fake_registers[4] = 0x0003;
            fake_registers[5] = 1;
            fake_registers[6] = 1;
            fake_registers[7] = 1;
            quantity = RS485_MODBUS_CAPABILITY_ASSIGN_REGS;
            rs485_store_capability(dst, fake_registers, quantity);
        } else {
            fake_registers[0] = 250;
        }

        rs485_fill_compat_response(dst, cmd, seq, fake_registers, quantity, response);
        rs485_mark_rx();
        rs485_mark_slave_success(dst);
        result.ok = true;
    } else {
        rs485_mark_result_error(dst, debug_forced_rx_result);
    }
}

RS485TransactionResult rs485_send_transaction(uint8_t dst,
                                             uint8_t cmd,
                                             const uint8_t* payload,
                                             uint8_t len,
                                             RS485Frame* response) {
    RS485TransactionResult result = {};
    result.ok = false;
    result.rx_result = RS485_RX_ERROR;
    result.dst = dst;
    result.cmd = cmd;
    result.response_type = 0;
    result.error_code = 0;

    ModbusRequestPlan plan = {};
    if (!rs485_plan_for_command(cmd, payload, len, plan) || dst == RS485_BROADCAST_ADDR) {
        result.rx_result = RS485_RX_ERROR;
        result.seq = next_seq++;
        result.attempts = 0;
        last_transaction = result;
        Serial.printf("[RS485] Modbus unsupported cmd=0x%02X dst=0x%02X\n", cmd, dst);
        rs485_mark_result_error(dst, result.rx_result);
        return result;
    }

    uint8_t base_seq = next_seq++;
    uint16_t registers[RS485_MAX_PAYLOAD / 2] = {0};
    if (plan.quantity > (RS485_MAX_PAYLOAD / 2)) plan.quantity = RS485_MAX_PAYLOAD / 2;

    for (uint8_t attempt = 0; attempt <= RS485_RETRY_COUNT; attempt++) {
        result.seq = base_seq + attempt;
        result.attempts = attempt + 1;

        while (rs485_serial.available()) rs485_serial.read();
        rs485_mark_tx();
        Serial.printf("[RS485] Modbus TX dst=0x%02X fc=0x%02X reg=0x%04X qty=%u seq=%u attempt=%u label=%s\n",
                      dst,
                      plan.function_code,
                      plan.reg,
                      plan.quantity,
                      result.seq,
                      attempt,
                      plan.label);

        if (debug_force_rx_result) {
            rs485_prepare_for_debug_force(dst, cmd, result.seq, result, response);
            last_transaction = result;
            Serial.printf("[RS485] Modbus forced %s dst=0x%02X cmd=0x%02X\n",
                          rs485_rx_result_name(result.rx_result),
                          dst,
                          cmd);
            return result;
        }

        uint8_t modbus_code = 0;
        if (plan.write) {
            modbus_code = rs485_modbus.writeHoldingRegister(dst, plan.reg, plan.value);
            registers[0] = plan.value;
        } else {
            modbus_code = rs485_modbus.readHoldingRegister(dst, plan.reg, registers, plan.quantity);
        }

        result.rx_result = rs485_result_from_modbus_code(modbus_code);
        result.error_code = modbus_code;
        result.response_type = result.rx_result == RS485_RX_OK ? RS485_TYPE_RESPONSE : RS485_TYPE_ERROR;

        if (result.rx_result == RS485_RX_OK) {
            rs485_mark_rx();
            rs485_mark_slave_success(dst);
            rs485_fill_compat_response(dst, cmd, result.seq, registers, plan.quantity, response);
            if (cmd == RS485_CMD_GET_INFO) {
                rs485_store_identity(dst, registers, plan.quantity);
            } else if (cmd == RS485_CMD_GET_CONFIG) {
                rs485_store_capability(dst, registers, plan.quantity);
            }
            result.ok = true;
            last_transaction = result;
            Serial.printf("[RS485] Modbus RX OK dst=0x%02X fc=0x%02X code=%u seq=%u\n",
                          dst,
                          plan.function_code,
                          modbus_code,
                          result.seq);
            return result;
        }

        Serial.printf("[RS485] Modbus RX %s dst=0x%02X fc=0x%02X code=%u seq=%u attempt=%u\n",
                      rs485_rx_result_name(result.rx_result),
                      dst,
                      plan.function_code,
                      modbus_code,
                      result.seq,
                      attempt);
        rs485_mark_result_error(dst, result.rx_result);
    }

    last_transaction = result;
    return result;
}

bool rs485_send_request(uint8_t dst, uint8_t cmd, const uint8_t* payload, uint8_t len, RS485Frame* response) {
    RS485TransactionResult result = rs485_send_transaction(dst, cmd, payload, len, response);
    return result.ok;
}

void rs485_request_pairing() {
    data_lock(g_state);
    rs485_clear_pairing_candidate_locked();
    g_state.rs485.pairing_requested = true;
    g_state.rs485.pairing_active = false;
    g_state.rs485.pairing_timeout_ms = RS485_PAIRING_TIMEOUT_MS;
    strncpy(g_state.rs485.status, "RS485 Modbus pairing requested", sizeof(g_state.rs485.status) - 1);
    g_state.rs485.status[sizeof(g_state.rs485.status) - 1] = '\0';
    data_unlock(g_state);
}

void rs485_cancel_pairing() {
    data_lock(g_state);
    g_state.rs485.pairing_requested = false;
    g_state.rs485.pairing_active = false;
    g_state.rs485.poll_enabled = pairing_restore_poll_enabled;
    rs485_clear_pairing_candidate_locked();
    strncpy(g_state.rs485.status, "RS485 pairing cancelled", sizeof(g_state.rs485.status) - 1);
    g_state.rs485.status[sizeof(g_state.rs485.status) - 1] = '\0';
    g_state.ui_needs_update = true;
    data_unlock(g_state);

    Serial.printf("[RS485] Pairing cancelled restore_poll=%u\n", pairing_restore_poll_enabled);
}

void rs485_request_assign_pairing_candidate(uint8_t address) {
    data_lock(g_state);
    if (!g_state.rs485.pairing_candidate_ready) {
        strncpy(g_state.rs485.status, "No pairing candidate", sizeof(g_state.rs485.status) - 1);
        g_state.rs485.status[sizeof(g_state.rs485.status) - 1] = '\0';
    } else {
        g_state.rs485.pairing_assign_requested = true;
        g_state.rs485.pairing_assign_address = address;
        snprintf(g_state.rs485.status, sizeof(g_state.rs485.status),
                 "Assign candidate%s", address == 0 ? "" : " requested");
    }
    g_state.ui_needs_update = true;
    data_unlock(g_state);
}

void rs485_set_poll_enabled(bool enabled) {
    data_lock(g_state);
    g_state.rs485.poll_enabled = enabled;
    strncpy(g_state.rs485.status,
            enabled ? "RS485 Modbus polling enabled" : "RS485 Modbus polling paused",
            sizeof(g_state.rs485.status) - 1);
    g_state.rs485.status[sizeof(g_state.rs485.status) - 1] = '\0';
    g_state.ui_needs_update = true;
    data_unlock(g_state);

    Serial.printf("[RS485] Poll %s\n", enabled ? "enabled" : "paused");
}

void rs485_request_test(uint8_t address, uint8_t cmd, bool write_command) {
    data_lock(g_state);
    if (g_state.rs485.pairing_active) {
        strncpy(g_state.rs485.test_status, "Finish/cancel pairing first", sizeof(g_state.rs485.test_status) - 1);
        g_state.rs485.test_status[sizeof(g_state.rs485.test_status) - 1] = '\0';
    } else {
        g_state.rs485.test_requested = true;
        g_state.rs485.test_write = write_command;
        g_state.rs485.test_address = address;
        g_state.rs485.test_cmd = cmd;
        g_state.rs485.test_ok = false;
        snprintf(g_state.rs485.test_status, sizeof(g_state.rs485.test_status),
                 "Queued Modbus 0x%02X", address);
    }
    g_state.ui_needs_update = true;
    data_unlock(g_state);
}

void rs485_request_light_command(bool on) {
    rs485_request_light_channel_command(0, on);
}

void rs485_request_light_channel_command(uint8_t channel, bool on) {
    if (channel > 2) return;
    data_lock(g_state);
    g_state.rs485.light_command_requested = true;
    g_state.rs485.light_command_on = on;
    g_state.rs485.light_command_channel = channel;
    if (channel == 0) {
        strncpy(g_state.rs485.status, on ? "Queued all lights ON" : "Queued all lights OFF",
                sizeof(g_state.rs485.status) - 1);
    } else {
        snprintf(g_state.rs485.status, sizeof(g_state.rs485.status),
                 "Queued LED %u %s", channel, on ? "ON" : "OFF");
    }
    g_state.rs485.status[sizeof(g_state.rs485.status) - 1] = '\0';
    g_state.ui_needs_update = true;
    data_unlock(g_state);
}

void rs485_request_ac_command(bool power, float target_c, uint8_t mode, uint8_t fan_speed, uint8_t swing_mode) {
    data_lock(g_state);
    g_state.rs485.ac_command_requested = true;
    g_state.rs485.ac_command_power = power;
    g_state.rs485.ac_command_target_c = target_c;
    g_state.rs485.ac_command_mode = mode;
    g_state.rs485.ac_command_fan_speed = fan_speed;
    g_state.rs485.ac_command_swing_mode = swing_mode;
    strncpy(g_state.rs485.status, "Queued AC command", sizeof(g_state.rs485.status) - 1);
    g_state.rs485.status[sizeof(g_state.rs485.status) - 1] = '\0';
    g_state.ui_needs_update = true;
    data_unlock(g_state);
}

static RS485SlaveState* rs485_projector_lux_slave_locked() {
    const LogicalMapping& mapping = g_state.rs485.mappings[LOGICAL_PROJECTOR_CONTROL];
    if (!mapping.assigned) return nullptr;

    uint8_t count = g_state.rs485.slave_count;
    if (count > RS485_MAX_SLAVES) count = RS485_MAX_SLAVES;
    for (uint8_t i = 0; i < count; i++) {
        RS485SlaveState& slave = g_state.rs485.slaves[i];
        bool mapping_matches = mapping.slave_uid != 0
                                   ? slave.uid == mapping.slave_uid
                                   : slave.address == mapping.slave_addr;
        if (!mapping_matches) continue;
        if (!slave.online ||
            !(slave.enabled_mask & RS485_CAP_PROJECTOR_IR) ||
            !(slave.enabled_mask & RS485_CAP_LUX) ||
            !slave.lux_valid) {
            return nullptr;
        }
        return &slave;
    }
    return nullptr;
}

static uint32_t rs485_projector_lux_source_key(const RS485SlaveState& slave) {
    return slave.uid != 0 ? slave.uid : (0xFF000000UL | slave.address);
}

static void rs485_reset_projector_lux_baseline_locked(uint32_t source_key) {
    g_state.sensor.proj_lux_source_key = source_key;
    g_state.sensor.proj_lux_baseline_avg = -1.0f;
    g_state.sensor.proj_lux_baseline_valid = false;
    memset(g_state.sensor.proj_lux_baseline, 0, sizeof(g_state.sensor.proj_lux_baseline));
    memset(g_state.sensor.proj_lux_baseline_channel_valid, 0,
           sizeof(g_state.sensor.proj_lux_baseline_channel_valid));
}

void rs485_request_projector_command(bool power, uint8_t input) {
    data_lock(g_state);
    Serial.printf("[Projector] Queue command target=%s previous=%s verif=%u\n",
                  power ? "ON" : "OFF",
                  g_state.sensor.projector_on ? "ON" : "OFF",
                  g_state.sensor.proj_verif_state);
    g_state.rs485.projector_command_requested = true;
    g_state.rs485.projector_command_power = power;
    g_state.rs485.projector_command_input = input;

    if (power) {
        RS485SlaveState* projector_lux_slave = rs485_projector_lux_slave_locked();
        uint32_t source_key = projector_lux_slave ? rs485_projector_lux_source_key(*projector_lux_slave) : 0;
        if (source_key != g_state.sensor.proj_lux_source_key) {
            rs485_reset_projector_lux_baseline_locked(source_key);
        }
        bool baseline_available = false;
        for (uint8_t i = 0; projector_lux_slave && i < 4; i++) {
            if (projector_lux_slave->lux_channel_valid[i] &&
                !g_state.sensor.proj_lux_baseline_channel_valid[i]) {
                g_state.sensor.proj_lux_baseline[i] = projector_lux_slave->lux_channel[i];
                g_state.sensor.proj_lux_baseline_channel_valid[i] = true;
            }
            if (projector_lux_slave->lux_channel_valid[i] &&
                g_state.sensor.proj_lux_baseline_channel_valid[i]) {
                baseline_available = true;
            }
        }
        g_state.sensor.projector_on = true;
        g_state.sensor.proj_warning_until_ms = 0;
        if (!baseline_available) {
            g_state.sensor.proj_verif_state = 4; // NO_LUX
            g_state.sensor.proj_hardware_failed = false;
        } else {
            g_state.sensor.proj_verif_state = 1; // POWERING_ON
            g_state.sensor.proj_warmup_timer_ms = millis() + 8000;
            g_state.sensor.proj_retry_count = 0;
            g_state.sensor.proj_hardware_failed = false;
        }
    } else {
        g_state.sensor.projector_on = false;
        g_state.sensor.proj_verif_state = 0; // OFF
        g_state.sensor.proj_hardware_failed = false;
        g_state.sensor.proj_warning_until_ms = 0;
        g_state.sensor.proj_retry_count = 0;
    }

    strncpy(g_state.rs485.status, "Queued projector command", sizeof(g_state.rs485.status) - 1);
    g_state.rs485.status[sizeof(g_state.rs485.status) - 1] = '\0';
    g_state.ui_needs_update = true;
    data_unlock(g_state);
}

static bool rs485_projector_lux_verified(float baseline_lux, float current_lux, float* delta_out, float* threshold_out, float* ratio_out) {
    float safe_baseline = baseline_lux;
    if (safe_baseline < 1.0f) safe_baseline = 1.0f;

    float delta_lux = current_lux - baseline_lux;
    float threshold_lux = baseline_lux * 0.20f;
    if (threshold_lux < 20.0f) threshold_lux = 20.0f;
    if (threshold_lux > 80.0f) threshold_lux = 80.0f;
    float ratio = current_lux / safe_baseline;

    if (delta_out) *delta_out = delta_lux;
    if (threshold_out) *threshold_out = threshold_lux;
    if (ratio_out) *ratio_out = ratio;

    return (delta_lux >= threshold_lux) || (ratio >= 1.25f && delta_lux >= 15.0f);
}

struct ProjectorLuxEval {
    uint8_t baseline_channels;
    uint8_t current_channels;
    uint8_t verified_channels;
    bool has_warning_channel;
    float best_delta;
    float best_threshold;
    float best_ratio;
    uint8_t best_channel;
};

static ProjectorLuxEval rs485_projector_eval_lux_locked() {
    ProjectorLuxEval eval = {};
    eval.best_delta = -100000.0f;
    eval.best_channel = 0xFF;
    RS485SlaveState* projector_lux_slave = rs485_projector_lux_slave_locked();
    if (!projector_lux_slave ||
        rs485_projector_lux_source_key(*projector_lux_slave) != g_state.sensor.proj_lux_source_key) {
        return eval;
    }

    for (uint8_t i = 0; i < 4; i++) {
        if (!g_state.sensor.proj_lux_baseline_channel_valid[i]) continue;
        eval.baseline_channels++;

        if (!projector_lux_slave->lux_channel_valid[i]) {
            eval.has_warning_channel = true;
            continue;
        }

        eval.current_channels++;
        float delta = 0.0f;
        float threshold = 0.0f;
        float ratio = 0.0f;
        bool verified = rs485_projector_lux_verified(g_state.sensor.proj_lux_baseline[i],
                                                     projector_lux_slave->lux_channel[i],
                                                     &delta,
                                                     &threshold,
                                                     &ratio);
        if (delta > eval.best_delta) {
            eval.best_delta = delta;
            eval.best_threshold = threshold;
            eval.best_ratio = ratio;
            eval.best_channel = i;
        }
        if (verified) eval.verified_channels++;
        else eval.has_warning_channel = true;
    }

    return eval;
}

static void rs485_request_pairing_debug(uint32_t timeout_ms) {
    if (timeout_ms == 0) timeout_ms = RS485_PAIRING_TIMEOUT_MS;
    debug_pairing_timeout_override_ms = timeout_ms;
    rs485_request_pairing();
}

static void rs485_handle_pairing_request() {
    bool requested = false;
    uint32_t timeout_ms = RS485_PAIRING_TIMEOUT_MS;

    data_lock(g_state);
    requested = g_state.rs485.pairing_requested;
    if (requested) {
        g_state.rs485.pairing_requested = false;
        g_state.rs485.pairing_active = true;
        g_state.rs485.pairing_started_ms = millis();
        pairing_known_scan_index = 0;
        if (debug_pairing_timeout_override_ms > 0) {
            g_state.rs485.pairing_timeout_ms = debug_pairing_timeout_override_ms;
            debug_pairing_timeout_override_ms = 0;
        }
        if (g_state.rs485.pairing_timeout_ms == 0) {
            g_state.rs485.pairing_timeout_ms = RS485_PAIRING_TIMEOUT_MS;
        }
        timeout_ms = g_state.rs485.pairing_timeout_ms;
        pairing_restore_poll_enabled = g_state.rs485.poll_enabled;
        g_state.rs485.poll_enabled = false;
        snprintf(g_state.rs485.status, sizeof(g_state.rs485.status),
                 "RS485 pair addr %u %lus",
                 RS485_MODBUS_PAIRING_ADDR,
                 (unsigned long)(timeout_ms / 1000));
        g_state.ui_needs_update = true;
    }
    data_unlock(g_state);

    if (requested) {
        Serial.printf("[RS485] Modbus pairing window addr=%u timeout=%lums restore_poll=%u\n",
                      RS485_MODBUS_PAIRING_ADDR,
                      (unsigned long)timeout_ms,
                      pairing_restore_poll_enabled);
    }
}

static void rs485_handle_pairing_assign_request() {
    bool requested = false;
    uint8_t address = 0;

    data_lock(g_state);
    requested = g_state.rs485.pairing_assign_requested;
    if (requested) {
        g_state.rs485.pairing_assign_requested = false;
        address = g_state.rs485.pairing_assign_address;
        strncpy(g_state.rs485.status, "Assigning candidate", sizeof(g_state.rs485.status) - 1);
        g_state.rs485.status[sizeof(g_state.rs485.status) - 1] = '\0';
        g_state.ui_needs_update = true;
    }
    data_unlock(g_state);

    if (!requested) return;

    if (!rs485_assign_pairing_candidate(address)) {
        data_lock(g_state);
        strncpy(g_state.rs485.status, "Pair assign failed", sizeof(g_state.rs485.status) - 1);
        g_state.rs485.status[sizeof(g_state.rs485.status) - 1] = '\0';
        g_state.ui_needs_update = true;
        data_unlock(g_state);
    }
}

static void rs485_handle_pairing_scan() {
    bool active = false;
    bool candidate_ready = false;
    uint32_t pairing_started_ms = 0;
    data_lock(g_state);
    active = g_state.rs485.pairing_active;
    candidate_ready = g_state.rs485.pairing_candidate_ready;
    pairing_started_ms = g_state.rs485.pairing_started_ms;
    data_unlock(g_state);

    if (!active || candidate_ready) return;

    uint32_t now = millis();
    if (now - last_pairing_scan_ms < RS485_PAIRING_SCAN_INTERVAL_MS) return;
    last_pairing_scan_ms = now;

    if (pairing_started_ms != 0 &&
        now - pairing_started_ms < RS485_PAIRING_KNOWN_SCAN_WINDOW_MS &&
        rs485_scan_known_slave_during_pairing()) {
        return;
    }

    rs485_scan_pairing_candidate();
}

static void rs485_handle_pairing_timeout() {
    bool timed_out = false;
    uint32_t timeout_ms = 0;
    uint32_t elapsed_ms = 0;

    data_lock(g_state);
    if (g_state.rs485.pairing_active) {
        timeout_ms = g_state.rs485.pairing_timeout_ms;
        if (timeout_ms == 0) timeout_ms = RS485_PAIRING_TIMEOUT_MS;
        elapsed_ms = millis() - g_state.rs485.pairing_started_ms;
        timed_out = elapsed_ms >= timeout_ms;
        if (timed_out) {
            g_state.rs485.pairing_active = false;
            g_state.rs485.poll_enabled = pairing_restore_poll_enabled;
            if (g_state.rs485.pairing_timeouts < UINT16_MAX) g_state.rs485.pairing_timeouts++;
            rs485_clear_pairing_candidate_locked();
            strncpy(g_state.rs485.status, "RS485 pairing timeout", sizeof(g_state.rs485.status) - 1);
            g_state.rs485.status[sizeof(g_state.rs485.status) - 1] = '\0';
            g_state.ui_needs_update = true;
        }
    }
    data_unlock(g_state);

    if (timed_out) {
        Serial.printf("[RS485] Pairing timeout elapsed=%lums restore_poll=%u\n",
                      (unsigned long)elapsed_ms,
                      pairing_restore_poll_enabled);
    }
}

static void rs485_debug_print_stats() {
    data_lock(g_state);
    Serial.println("[RS485DBG] Modbus stats");
    uint32_t pairing_remaining_ms = 0;
    if (g_state.rs485.pairing_active) {
        uint32_t timeout_ms = g_state.rs485.pairing_timeout_ms;
        if (timeout_ms == 0) timeout_ms = RS485_PAIRING_TIMEOUT_MS;
        uint32_t elapsed_ms = millis() - g_state.rs485.pairing_started_ms;
        pairing_remaining_ms = elapsed_ms >= timeout_ms ? 0 : timeout_ms - elapsed_ms;
    }
    Serial.printf("[RS485DBG] init=%u bus_ok=%u pairing=%u poll=%u slaves=%u pair_addr=%u pair_remain=%lums pair_timeouts=%u\n",
                  g_state.rs485.initialized,
                  g_state.rs485.bus_ok,
                  g_state.rs485.pairing_active,
                  g_state.rs485.poll_enabled,
                  g_state.rs485.slave_count,
                  RS485_MODBUS_PAIRING_ADDR,
                  (unsigned long)pairing_remaining_ms,
                  g_state.rs485.pairing_timeouts);
    Serial.printf("[RS485DBG] tx=%lu rx=%lu crc=%lu timeout=%lu status=%s\n",
                  (unsigned long)g_state.rs485.packets_tx,
                  (unsigned long)g_state.rs485.packets_rx,
                  (unsigned long)g_state.rs485.crc_errors,
                  (unsigned long)g_state.rs485.timeout_errors,
                  g_state.rs485.status);
    for (uint8_t i = 0; i < g_state.rs485.slave_count && i < RS485_MAX_SLAVES; i++) {
        Serial.printf("[RS485DBG] slave[%u] addr=0x%02X role=0x%02X cap=0x%04X online=%u degraded=%u last_seen=%lu\n",
                      i,
                      g_state.rs485.slaves[i].address,
                      g_state.rs485.slaves[i].role,
                      g_state.rs485.slaves[i].capability,
                      g_state.rs485.slaves[i].online,
                      g_state.rs485.slaves[i].degraded,
                      (unsigned long)g_state.rs485.slaves[i].last_seen);
        Serial.printf("[RS485DBG]   ident=%u uid=0x%08lX mac=%012llX proto=%u class=%u fw=%u cap_sync=%u counts T:%u CO2:%u PRES:%u REL:%u IR:%u LUX:%u LCD:%u enabled=0x%04X\n",
                      g_state.rs485.slaves[i].identity_synced,
                      (unsigned long)g_state.rs485.slaves[i].uid,
                      (unsigned long long)g_state.rs485.slaves[i].mac,
                      g_state.rs485.slaves[i].protocol_version,
                      g_state.rs485.slaves[i].device_class,
                      g_state.rs485.slaves[i].fw_version,
                      g_state.rs485.slaves[i].capability_synced,
                      g_state.rs485.slaves[i].temp_count,
                      g_state.rs485.slaves[i].co2_count,
                      g_state.rs485.slaves[i].presence_count,
                      g_state.rs485.slaves[i].relay_count,
                      g_state.rs485.slaves[i].ir_count,
                      g_state.rs485.slaves[i].lux_count,
                      g_state.rs485.slaves[i].lcd_count,
                      g_state.rs485.slaves[i].enabled_mask);
        Serial.printf("[RS485DBG]   counters errors=%u fail=%u ok=%u crc=%u timeout=%u seq=%u len=%u nack=%u\n",
                      g_state.rs485.slaves[i].error_count,
                      g_state.rs485.slaves[i].consecutive_fail,
                      g_state.rs485.slaves[i].rx_success,
                      g_state.rs485.slaves[i].crc_errors,
                      g_state.rs485.slaves[i].timeout_errors,
                      g_state.rs485.slaves[i].seq_errors,
                      g_state.rs485.slaves[i].len_errors,
                      g_state.rs485.slaves[i].nack_count);
    }
    Serial.printf("[RS485DBG] dashboard temp_valid=%u%u%u%u co2=%u lux=%u human=%u ac=%u proj=%u\n",
                  g_state.rs485.dashboard.temp_valid[0],
                  g_state.rs485.dashboard.temp_valid[1],
                  g_state.rs485.dashboard.temp_valid[2],
                  g_state.rs485.dashboard.temp_valid[3],
                  g_state.rs485.dashboard.co2_valid,
                  g_state.rs485.dashboard.lux_valid,
                  g_state.rs485.dashboard.human_presence_valid,
                  g_state.rs485.dashboard.ac_available,
                  g_state.rs485.dashboard.projector_available);
    for (uint8_t i = 0; i < DASHBOARD_LOGICAL_SLOT_COUNT; i++) {
        const LogicalMapping& mapping = g_state.rs485.mappings[i];
        Serial.printf("[RS485DBG] map[%u] cap=0x%02X assigned=%u manual=%u uid=0x%08lX addr=0x%02X ch=%u\n",
                      i,
                      mapping.capability_type,
                      mapping.assigned,
                      mapping.manual_override,
                      (unsigned long)mapping.slave_uid,
                      mapping.slave_addr,
                      mapping.channel);
    }
    data_unlock(g_state);

    Serial.printf("[RS485DBG] last ok=%u result=%s dst=0x%02X cmd=0x%02X seq=%u attempts=%u type=0x%02X modbus_code=0x%02X\n",
                  last_transaction.ok,
                  rs485_rx_result_name(last_transaction.rx_result),
                  last_transaction.dst,
                  last_transaction.cmd,
                  last_transaction.seq,
                  last_transaction.attempts,
                  last_transaction.response_type,
                  last_transaction.error_code);
}

static void rs485_debug_help() {
    Serial.println("[RS485DBG] commands:");
    Serial.println("[RS485DBG]   rs485 stats");
    Serial.println("[RS485DBG]   rs485 poll on|off");
    Serial.println("[RS485DBG]   rs485 sync [dst_hex]");
    Serial.println("[RS485DBG]   rs485 pair [timeout_ms]");
    Serial.println("[RS485DBG]   rs485 pairscan");
    Serial.println("[RS485DBG]   rs485 pairfake");
    Serial.println("[RS485DBG]   rs485 assign [new_addr_hex_or_0_auto]");
    Serial.println("[RS485DBG]   rs485 paircancel");
    Serial.println("[RS485DBG]   rs485 save");
    Serial.println("[RS485DBG]   rs485 inject ok|timeout|crc|seq|cmd|addr|nack|error");
    Serial.println("[RS485DBG]   rs485 test [dst_hex] [cmd_hex]");
    Serial.println("[RS485DBG]   rs485 testwrite [dst_hex]");
    Serial.println("[RS485DBG]   rs485 relay <dst_hex> <channel_1_or_2> <0_or_1>");
    Serial.println("[RS485DBG]   rs485 testforce ok|timeout|crc|seq|cmd|addr|nack|error [dst_hex] [cmd_hex]");
    Serial.println("[RS485DBG] Modbus map v2.1: IDENTITY->0x0000..0x0004, CONFIG->0x0010..0x0017, RUNTIME->0x0100..0x010E");
}

static void rs485_debug_run_sync(char* addr_token) {
    uint8_t dst = addr_token ? (uint8_t)strtoul(addr_token, nullptr, 0) : 0x10;
    uint16_t identity[RS485_MODBUS_IDENTITY_REGS] = {0};
    uint16_t capability[RS485_MODBUS_CAPABILITY_ASSIGN_REGS] = {0};
    bool identity_ok = rs485_read_holding_registers(dst,
                                                    RS485_MODBUS_REG_NODE_ADDRESS,
                                                    RS485_MODBUS_IDENTITY_REGS,
                                                    identity,
                                                    "IDENTITY_SYNC_MANUAL");
    if (identity_ok) rs485_store_identity(dst, identity, RS485_MODBUS_IDENTITY_REGS);
    bool capability_ok = rs485_read_holding_registers(dst,
                                                      RS485_MODBUS_REG_TEMP_ASSIGNMENT,
                                                      RS485_MODBUS_CAPABILITY_ASSIGN_REGS,
                                                      capability,
                                                      "CAPABILITY_SYNC_MANUAL");
    if (capability_ok) rs485_store_capability(dst, capability, RS485_MODBUS_CAPABILITY_ASSIGN_REGS);
    Serial.printf("[RS485DBG] sync dst=0x%02X identity=%u capability=%u\n",
                  dst,
                  identity_ok,
                  capability_ok);
}

static void rs485_debug_run_test(char* addr_token, char* cmd_token) {
    uint8_t dst = addr_token ? (uint8_t)strtoul(addr_token, nullptr, 0) : 0x10;
    uint8_t cmd = cmd_token ? (uint8_t)strtoul(cmd_token, nullptr, 0) : RS485_CMD_READ_SENSOR;
    RS485Frame response = {};
    RS485TransactionResult result = rs485_send_transaction(dst, cmd, nullptr, 0, &response);

    Serial.printf("[RS485DBG] test result ok=%u rx=%s dst=0x%02X cmd=0x%02X seq=%u attempts=%u len=%u\n",
                  result.ok,
                  rs485_rx_result_name(result.rx_result),
                  result.dst,
                  result.cmd,
                  result.seq,
                  result.attempts,
                  response.len);
}

static void rs485_debug_run_write_test(char* addr_token) {
    uint8_t dst = addr_token ? (uint8_t)strtoul(addr_token, nullptr, 0) : 0x10;
    uint8_t payload[2] = {0, 1};
    RS485Frame response = {};
    RS485TransactionResult result = rs485_send_transaction(dst, RS485_CMD_SET_OUTPUT, payload, sizeof(payload), &response);

    Serial.printf("[RS485DBG] write test result ok=%u rx=%s dst=0x%02X seq=%u attempts=%u\n",
                  result.ok,
                  rs485_rx_result_name(result.rx_result),
                  result.dst,
                  result.seq,
                  result.attempts);
}

static void rs485_debug_set_poll(char* mode_token) {
    if (!mode_token) {
        Serial.println("[RS485DBG] invalid poll value");
        rs485_debug_help();
        return;
    }

    bool enabled = false;
    if (strcasecmp(mode_token, "on") == 0 || strcasecmp(mode_token, "1") == 0) {
        enabled = true;
    } else if (strcasecmp(mode_token, "off") == 0 || strcasecmp(mode_token, "0") == 0) {
        enabled = false;
    } else {
        Serial.println("[RS485DBG] invalid poll value");
        rs485_debug_help();
        return;
    }

    rs485_set_poll_enabled(enabled);
}

static bool rs485_debug_set_forced_result(char* token) {
    RS485RxResult result = RS485_RX_TIMEOUT;
    if (!rs485_parse_rx_result(token, result)) {
        Serial.println("[RS485DBG] invalid RX result value");
        rs485_debug_help();
        return false;
    }

    debug_forced_rx_result = result;
    debug_force_rx_result = true;
    Serial.printf("[RS485DBG] next Modbus RX forced to %s\n", rs485_rx_result_name(result));
    return true;
}

static void rs485_debug_write_relay(char* addr_token, char* channel_token, char* value_token) {
    if (!addr_token || !channel_token || !value_token) {
        Serial.println("[RS485DBG] usage: rs485 relay <dst_hex> <channel_1_or_2> <0_or_1>");
        return;
    }

    uint8_t dst = (uint8_t)strtoul(addr_token, nullptr, 0);
    uint8_t channel = (uint8_t)strtoul(channel_token, nullptr, 0);
    uint16_t value = (uint16_t)strtoul(value_token, nullptr, 0);
    if (dst == 0 || channel < 1 || channel > 2 || value > 1) {
        Serial.println("[RS485DBG] invalid relay command");
        return;
    }

    uint16_t reg = channel == 1 ? RS485_MODBUS_REG_RELAY_1 : RS485_MODBUS_REG_RELAY_2;
    bool ok = rs485_write_holding_register(dst, reg, value, "RELAY_DEBUG_DIRECT");
    Serial.printf("[RS485DBG] relay dst=0x%02X channel=%u value=%u ok=%u\n",
                  dst, channel, value, ok);
}

static void rs485_debug_process_line(char* line) {
    char* cmd = strtok(line, " \t");
    if (!cmd) return;

    if (strcasecmp(cmd, "k") == 0) {
        screens_set(SCREEN_TOUCH_TEST);
        Serial.println("[TC] Touch alignment test opened.");
        return;
    }

    if (strcasecmp(cmd, "b") == 0) {
        screens_set(SCREEN_DASHBOARD);
        Serial.println("[TC] Returned to dashboard.");
        return;
    }

    if (strcasecmp(cmd, "rs485") != 0) return;

    char* sub = strtok(nullptr, " \t");
    if (!sub || strcasecmp(sub, "help") == 0) {
        rs485_debug_help();
        return;
    }

    if (strcasecmp(sub, "stats") == 0) {
        rs485_debug_print_stats();
        return;
    }

    if (strcasecmp(sub, "poll") == 0) {
        rs485_debug_set_poll(strtok(nullptr, " \t"));
        return;
    }

    if (strcasecmp(sub, "sync") == 0) {
        rs485_debug_run_sync(strtok(nullptr, " \t"));
        return;
    }

    if (strcasecmp(sub, "pair") == 0) {
        char* timeout_token = strtok(nullptr, " \t");
        uint32_t timeout_ms = timeout_token ? strtoul(timeout_token, nullptr, 0) : RS485_PAIRING_TIMEOUT_MS;
        rs485_request_pairing_debug(timeout_ms);
        Serial.printf("[RS485DBG] pairing requested timeout=%lums\n", (unsigned long)timeout_ms);
        return;
    }

    if (strcasecmp(sub, "pairscan") == 0) {
        bool ok = rs485_scan_pairing_candidate();
        Serial.printf("[RS485DBG] pairscan ok=%u addr=%u\n", ok, RS485_MODBUS_PAIRING_ADDR);
        return;
    }

    if (strcasecmp(sub, "pairfake") == 0) {
        rs485_seed_fake_pairing_candidate();
        Serial.println("[RS485DBG] fake pairing candidate ready");
        return;
    }

    if (strcasecmp(sub, "assign") == 0) {
        char* addr_token = strtok(nullptr, " \t");
        uint8_t addr = addr_token ? (uint8_t)strtoul(addr_token, nullptr, 0) : 0;
        bool ok = rs485_assign_pairing_candidate(addr);
        Serial.printf("[RS485DBG] assign ok=%u requested=0x%02X\n", ok, addr);
        return;
    }

    if (strcasecmp(sub, "paircancel") == 0) {
        rs485_cancel_pairing();
        return;
    }

    if (strcasecmp(sub, "save") == 0) {
        data_save_rs485_config(g_state);
        Serial.println("[RS485DBG] RS485 registry/mapping saved");
        return;
    }

    if (strcasecmp(sub, "inject") == 0) {
        rs485_debug_set_forced_result(strtok(nullptr, " \t"));
        return;
    }

    if (strcasecmp(sub, "test") == 0) {
        char* addr_token = strtok(nullptr, " \t");
        char* cmd_token = strtok(nullptr, " \t");
        rs485_debug_run_test(addr_token, cmd_token);
        return;
    }

    if (strcasecmp(sub, "testwrite") == 0) {
        rs485_debug_run_write_test(strtok(nullptr, " \t"));
        return;
    }

    if (strcasecmp(sub, "relay") == 0) {
        char* addr_token = strtok(nullptr, " \t");
        char* channel_token = strtok(nullptr, " \t");
        char* value_token = strtok(nullptr, " \t");
        rs485_debug_write_relay(addr_token, channel_token, value_token);
        return;
    }

    if (strcasecmp(sub, "testforce") == 0) {
        char* result_token = strtok(nullptr, " \t");
        char* addr_token = strtok(nullptr, " \t");
        char* cmd_token = strtok(nullptr, " \t");
        if (rs485_debug_set_forced_result(result_token)) {
            rs485_debug_run_test(addr_token, cmd_token);
        }
        return;
    }

    rs485_debug_help();
}

static void rs485_debug_serial_loop() {
    while (Serial.available()) {
        char c = (char)Serial.read();

        if ((c == 'k' || c == 'K') && debug_line_len == 0) {
            screens_set(SCREEN_TOUCH_TEST);
            Serial.println("[TC] Touch alignment test opened.");
            continue;
        }

        if ((c == 'b' || c == 'B') && debug_line_len == 0) {
            screens_set(SCREEN_DASHBOARD);
            Serial.println("[TC] Returned to dashboard.");
            continue;
        }

        if (c == '\r') continue;
        if (c == '\n') {
            debug_line[debug_line_len] = '\0';
            rs485_debug_process_line(debug_line);
            debug_line_len = 0;
            debug_line[0] = '\0';
            continue;
        }

        if (debug_line_len < sizeof(debug_line) - 1) {
            debug_line[debug_line_len++] = c;
        } else {
            debug_line_len = 0;
            debug_line[0] = '\0';
            Serial.println("[RS485DBG] command too long");
        }
    }
}

static void rs485_poll_one_slave() {
    bool poll_enabled = false;
    uint8_t slave_count = 0;
    uint8_t slave_index = RS485_MAX_SLAVES;
    uint8_t address = 0;
    bool identity_due = false;
    bool capability_due = false;
    bool sensor_poll_pending = false;
    uint32_t now = millis();

    data_lock(g_state);
    poll_enabled = g_state.rs485.poll_enabled;
    slave_count = g_state.rs485.slave_count;
    if (slave_count > RS485_MAX_SLAVES) slave_count = RS485_MAX_SLAVES;
    if (poll_enabled && slave_count > 0 && !g_state.rs485.pairing_active) {
        if (poll_index >= slave_count) poll_index = 0;
        RS485SlaveState& slave = g_state.rs485.slaves[poll_index];
        if (slave.uid != RS485_DUMMY_UI_UID) {
            slave_index = poll_index;
            address = slave.address;
            sensor_poll_pending = slave.sensor_poll_pending;
            identity_due = !slave.identity_synced ||
                           now - slave.last_identity_ms >= RS485_IDENTITY_SYNC_INTERVAL_MS;
            capability_due = !slave.capability_synced ||
                             now - slave.last_capability_ms >= RS485_CAPABILITY_SYNC_INTERVAL_MS;
        }
        poll_index++;
    }
    data_unlock(g_state);

    if (!poll_enabled || slave_count == 0 || address == 0) return;

    data_lock(g_state);
    bool boot_recovery_first = slave_index < RS485_MAX_SLAVES &&
                               g_state.rs485.slaves[slave_index].mac != 0 &&
                               g_state.rs485.slaves[slave_index].last_seen == 0 &&
                               !g_state.rs485.slaves[slave_index].online;
    data_unlock(g_state);
    if (boot_recovery_first) {
        rs485_try_auto_recovery(slave_index);
        return;
    }

    bool ok = false;
    if (sensor_poll_pending) {
        ok = rs485_poll_sensor_registers(address);
        if (!ok) rs485_try_auto_recovery(slave_index);
        return;
    }

    if (identity_due) {
        ok = rs485_sync_identity_if_needed(address);
        if (!ok) rs485_try_auto_recovery(slave_index);
        return;
    }
    if (capability_due) {
        ok = rs485_sync_capability_if_needed(address);
        if (!ok) rs485_try_auto_recovery(slave_index);
        return;
    }

    ok = rs485_poll_sensor_registers(address);
    if (!ok) rs485_try_auto_recovery(slave_index);
}

static void rs485_handle_ui_test_request() {
    bool requested = false;
    bool write_command = false;
    uint8_t address = 0;
    uint8_t cmd = 0;

    data_lock(g_state);
    requested = g_state.rs485.test_requested && !g_state.rs485.test_busy;
    if (requested) {
        g_state.rs485.test_requested = false;
        g_state.rs485.test_busy = true;
        g_state.rs485.test_started_ms = millis();
        address = g_state.rs485.test_address;
        cmd = g_state.rs485.test_cmd;
        write_command = g_state.rs485.test_write;
        snprintf(g_state.rs485.test_status, sizeof(g_state.rs485.test_status),
                 "Modbus 0x%02X cmd 0x%02X", address, cmd);
        g_state.ui_needs_update = true;
    }
    data_unlock(g_state);

    if (!requested || address == 0) return;

    uint8_t payload[2] = {0, 1};
    RS485Frame response = {};
    RS485TransactionResult result = rs485_send_transaction(address,
                                                          write_command ? RS485_CMD_SET_OUTPUT : cmd,
                                                          write_command ? payload : nullptr,
                                                          write_command ? sizeof(payload) : 0,
                                                          &response);
    if (result.ok && !write_command && cmd == RS485_CMD_READ_SENSOR) {
        rs485_update_sensor_from_modbus_payload(response.src, response.payload, response.len);
    }

    data_lock(g_state);
    g_state.rs485.test_busy = false;
    g_state.rs485.test_ok = result.ok;
    g_state.rs485.test_result = (uint8_t)result.rx_result;
    g_state.rs485.test_seq = result.seq;
    g_state.rs485.test_attempts = result.attempts;
    g_state.rs485.test_done_ms = millis();
    snprintf(g_state.rs485.test_status, sizeof(g_state.rs485.test_status),
             "%s 0x%02X %s seq:%u att:%u",
             write_command ? "WRITE" : "READ",
             address,
             rs485_rx_result_name(result.rx_result),
             result.seq,
             result.attempts);
    g_state.ui_needs_update = true;
    data_unlock(g_state);
}

static void rs485_handle_control_commands() {
    bool light_requested = false;
    bool light_on = false;
    uint8_t light_channel = 0;
    bool ac_requested = false;
    bool ac_power = false;
    float ac_target_c = 0.0f;
    uint8_t ac_mode = 0;
    uint8_t ac_fan_speed = 0;
    uint8_t ac_swing_mode = 0;
    bool projector_requested = false;
    bool projector_power = false;
    uint8_t projector_input = 0;

    data_lock(g_state);
    if (!g_state.rs485.pairing_active) {
        light_requested = g_state.rs485.light_command_requested;
        light_on = g_state.rs485.light_command_on;
        light_channel = g_state.rs485.light_command_channel;
        ac_requested = g_state.rs485.ac_command_requested;
        ac_power = g_state.rs485.ac_command_power;
        ac_target_c = g_state.rs485.ac_command_target_c;
        ac_mode = g_state.rs485.ac_command_mode;
        ac_fan_speed = g_state.rs485.ac_command_fan_speed;
        ac_swing_mode = g_state.rs485.ac_command_swing_mode;
        projector_requested = g_state.rs485.projector_command_requested;
        projector_power = g_state.rs485.projector_command_power;
        projector_input = g_state.rs485.projector_command_input;

        g_state.rs485.light_command_requested = false;
        g_state.rs485.light_command_channel = 0;
        g_state.rs485.ac_command_requested = false;
        g_state.rs485.projector_command_requested = false;
    }
    data_unlock(g_state);

    if (light_requested) {
        bool ok = rs485_write_light_command(light_channel, light_on);
        data_lock(g_state);
        g_state.rs485.light_command_failed = !ok;
        g_state.rs485.light_state_publish_pending = true;
        snprintf(g_state.rs485.status, sizeof(g_state.rs485.status),
                 ok ? "Light state confirmed by readback" : "Light command/readback failed");
        g_state.ui_needs_update = true;
        data_unlock(g_state);
        Serial.printf("[RS485] Light %s requested=%s channel=%u; MQTT confirmation queued\n",
                      ok ? "confirmed" : "failed",
                      light_on ? "ON" : "OFF",
                      light_channel);
    }

    if (ac_requested) {
        bool ok = rs485_write_ac_command(ac_power, ac_target_c, ac_mode, ac_fan_speed, ac_swing_mode);
        data_lock(g_state);
        snprintf(g_state.rs485.status, sizeof(g_state.rs485.status),
                 ok ? "AC command sent" : "AC command failed");
        g_state.ui_needs_update = true;
        data_unlock(g_state);
    }

    if (projector_requested) {
        bool ok = rs485_write_projector_command(projector_power, projector_input);
        data_lock(g_state);
        snprintf(g_state.rs485.status, sizeof(g_state.rs485.status),
                 ok ? "Projector command sent" : "Projector command failed");
        g_state.ui_needs_update = true;
        data_unlock(g_state);
    }
}

static void rs485_handle_projector_verification() {
    data_lock(g_state);
    if (!g_state.sensor.projector_on &&
        (g_state.sensor.proj_verif_state == 0 || g_state.sensor.proj_verif_state == 6)) {
        RS485SlaveState* projector_lux_slave = rs485_projector_lux_slave_locked();
        uint32_t source_key = projector_lux_slave ? rs485_projector_lux_source_key(*projector_lux_slave) : 0;
        if (source_key != g_state.sensor.proj_lux_source_key) {
            rs485_reset_projector_lux_baseline_locked(source_key);
        }
        uint32_t sum = 0;
        uint8_t count = 0;
        for (uint8_t i = 0; projector_lux_slave && i < 4; i++) {
            if (!projector_lux_slave->lux_channel_valid[i]) continue;
            float lux = projector_lux_slave->lux_channel[i];
            if (!g_state.sensor.proj_lux_baseline_channel_valid[i]) {
                g_state.sensor.proj_lux_baseline[i] = lux;
                g_state.sensor.proj_lux_baseline_channel_valid[i] = true;
            } else {
                g_state.sensor.proj_lux_baseline[i] =
                    (g_state.sensor.proj_lux_baseline[i] * 0.90f) + (lux * 0.10f);
            }
            sum += (uint32_t)lux;
            count++;
        }
        if (count > 0) {
            g_state.sensor.proj_lux_baseline_avg = (float)sum / count;
            g_state.sensor.proj_lux_baseline_valid = true;
        }
    }

    if (g_state.sensor.proj_verif_state == 6 &&
        g_state.sensor.proj_warning_until_ms != 0 &&
        (int32_t)(millis() - g_state.sensor.proj_warning_until_ms) >= 0) {
        g_state.sensor.proj_verif_state = 2; // VERIFIED_ON visual state after warning timeout
        g_state.sensor.proj_hardware_failed = false;
        g_state.sensor.proj_warning_until_ms = 0;
        g_state.ui_needs_update = true;
    }

    if (g_state.sensor.proj_verif_state == 1 || g_state.sensor.proj_verif_state == 3) {
        ProjectorLuxEval eval = rs485_projector_eval_lux_locked();
        if (eval.baseline_channels == 0 || eval.current_channels == 0) {
            g_state.sensor.proj_verif_state = 4; // NO_LUX
            g_state.sensor.proj_hardware_failed = false;
            g_state.ui_needs_update = true;
            data_unlock(g_state);
            return;
        }

        if (eval.verified_channels > 0) {
            g_state.sensor.proj_verif_state =
                (eval.has_warning_channel || eval.verified_channels < eval.baseline_channels) ? 5 : 2; // CHECK_LUX or VERIFIED_ON
            g_state.sensor.proj_hardware_failed = false;
            g_state.ui_needs_update = true;
            Serial.printf("[Projector] ON verified by lux ch%u. verified=%u/%u delta=%.1f threshold=%.1f ratio=%.2f state=%u\n",
                          eval.best_channel + 1,
                          eval.verified_channels,
                          eval.baseline_channels,
                          eval.best_delta,
                          eval.best_threshold,
                          eval.best_ratio,
                          g_state.sensor.proj_verif_state);
            data_unlock(g_state);
            return;
        }

        if (millis() >= g_state.sensor.proj_warmup_timer_ms) {
            Serial.printf("[Projector] Timer expired. verified=%u/%u best_ch=%u delta=%.1f threshold=%.1f ratio=%.2f\n",
                          eval.verified_channels,
                          eval.baseline_channels,
                          eval.best_channel == 0xFF ? 0 : eval.best_channel + 1,
                          eval.best_delta,
                          eval.best_threshold,
                          eval.best_ratio);
            
            if (g_state.sensor.proj_verif_state == 1) {
                g_state.sensor.proj_verif_state = 3; // RETRYING
                g_state.sensor.proj_warmup_timer_ms = millis() + 8000;
                g_state.sensor.proj_retry_count = 1;
                g_state.ui_needs_update = true;

                g_state.rs485.projector_command_requested = true;
                g_state.rs485.projector_command_power = true;
                g_state.rs485.projector_command_input = 0;

                Serial.println("[Projector] First verification failed. Retrying ON.");
            } else {
                g_state.sensor.proj_verif_state = 6; // CHECK_PROJECTOR
                g_state.sensor.proj_hardware_failed = true;
                g_state.sensor.projector_on = true;
                g_state.sensor.proj_warning_until_ms = millis() + 10000;
                g_state.ui_needs_update = true;

                Serial.println("[Projector] Verification failed after retry. Keeping ON with CHECK_PROJECTOR warning.");
            }
        }
    }
    data_unlock(g_state);
}

void rs485_manager_init() {
    pinMode(RS485_DIR_PIN, OUTPUT);
    digitalWrite(RS485_DIR_PIN, LOW);
    rs485_serial.begin(RS485_BAUDRATE, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
    rs485_modbus.setTimeoutTimeMs(RS485_RESPONSE_TIMEOUT_MS);

    data_lock(g_state);
    g_state.rs485.initialized = true;
    g_state.rs485.bus_ok = true;
    snprintf(g_state.rs485.status, sizeof(g_state.rs485.status),
             "RS485 Modbus init UART%d %ld bps", RS485_UART_NUM, (long)RS485_BAUDRATE);
    data_unlock(g_state);

    Serial.printf("[RS485] Modbus init UART%d TX:%d RX:%d DIR:%d baud:%ld timeout:%lums\n",
                  RS485_UART_NUM,
                  RS485_TX_PIN,
                  RS485_RX_PIN,
                  RS485_DIR_PIN,
                  (long)RS485_BAUDRATE,
                  (unsigned long)RS485_RESPONSE_TIMEOUT_MS);
}

void rs485_manager_loop() {
    rs485_debug_serial_loop();
    rs485_handle_pairing_request();
    rs485_handle_pairing_assign_request();
    rs485_handle_pairing_scan();
    rs485_handle_pairing_timeout();
    rs485_handle_ui_test_request();
    rs485_handle_projector_verification();
    rs485_handle_control_commands();

    uint32_t now = millis();
    if (now - last_poll_ms >= RS485_POLL_INTERVAL_MS) {
        last_poll_ms = now;
        rs485_poll_one_slave();
    }
}

static void Task_RS485(void* pvParameters) {
    (void)pvParameters;
    rs485_manager_init();

    for (;;) {
        rs485_manager_loop();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void rs485_task_init() {
    xTaskCreatePinnedToCore(Task_RS485, "Task_RS485", 4096, NULL, 1, NULL, 0);
}
