#include "mapping_manager.h"

#include <string.h>

static bool slave_has_enabled_capability(const RS485SlaveState& slave, uint16_t capability) {
    return slave.online &&
           slave.capability_synced &&
           (slave.capability & capability) &&
           (slave.enabled_mask & capability);
}

static bool slave_temp_channel_enabled(const RS485SlaveState& slave, uint8_t channel) {
    return slave_has_enabled_capability(slave, CAP_TEMP) &&
           channel < DASHBOARD_TEMP_SLOTS &&
           (slave.temp_available_mask & (1 << channel)) &&
           (slave.temp_enabled_mask & (1 << channel));
}

static bool is_ir_node(const RS485SlaveState& slave) {
    return slave.profile == IR_COMBO_NODE || 
           (slave.enabled_mask & (CAP_PROJECTOR_IR | CAP_AC_IR)) || 
           (slave.capability & (CAP_PROJECTOR_IR | CAP_AC_IR));
}

static RS485SlaveState* find_slave_by_mapping_locked(BuildingState& state, const LogicalMapping& mapping) {
    uint8_t count = state.rs485.slave_count;
    if (count > RS485_MAX_SLAVES) count = RS485_MAX_SLAVES;

    for (uint8_t i = 0; i < count; i++) {
        RS485SlaveState& slave = state.rs485.slaves[i];
        if (mapping.slave_uid != 0 && slave.uid == mapping.slave_uid) return &slave;
        if (mapping.slave_uid == 0 && mapping.slave_addr != 0 && slave.address == mapping.slave_addr) return &slave;
    }

    return nullptr;
}

static bool temp_channel_already_mapped(const BuildingState& state, uint32_t uid, uint8_t addr, uint8_t channel) {
    for (uint8_t i = LOGICAL_TEMP_SLOT_1; i <= LOGICAL_TEMP_SLOT_4; i++) {
        const LogicalMapping& mapping = state.rs485.mappings[i];
        if (!mapping.assigned) continue;
        if (mapping.channel != channel) continue;
        if (uid != 0 && mapping.slave_uid == uid) return true;
        if (uid == 0 && mapping.slave_addr == addr) return true;
    }
    return false;
}

static void assign_mapping(LogicalMapping& mapping, const RS485SlaveState& slave, uint8_t channel) {
    mapping.slave_uid = slave.uid;
    mapping.slave_addr = slave.address;
    mapping.channel = channel;
    mapping.assigned = true;
}

static bool mapping_source_usable_locked(BuildingState& state, const LogicalMapping& mapping) {
    if (!mapping.assigned || mapping.capability_type == 0) return false;

    RS485SlaveState* slave = find_slave_by_mapping_locked(state, mapping);
    if (!slave) return false;

    if (mapping.capability_type == CAP_TEMP) {
        return slave_temp_channel_enabled(*slave, mapping.channel);
    }

    if (mapping.logical_id == LOGICAL_LUX_MAIN) {
        if (is_ir_node(*slave)) {
            return false;
        }
    }

    return slave_has_enabled_capability(*slave, mapping.capability_type);
}

static bool mapping_source_configured_locked(BuildingState& state, const LogicalMapping& mapping) {
    if (!mapping.assigned || mapping.capability_type == 0) return false;

    RS485SlaveState* slave = find_slave_by_mapping_locked(state, mapping);
    if (!slave) return false;

    if (mapping.capability_type == CAP_TEMP) {
        return mapping.channel < DASHBOARD_TEMP_SLOTS &&
               (slave->enabled_mask & CAP_TEMP) &&
               (slave->temp_available_mask & (1 << mapping.channel)) &&
               (slave->temp_enabled_mask & (1 << mapping.channel));
    }

    if (mapping.logical_id == LOGICAL_LUX_MAIN) {
        if (is_ir_node(*slave)) {
            return false;
        }
    }

    return (slave->enabled_mask & mapping.capability_type) != 0;
}

static void clear_stale_auto_mappings_locked(BuildingState& state) {
    for (uint8_t i = 0; i < DASHBOARD_LOGICAL_SLOT_COUNT; i++) {
        LogicalMapping& mapping = state.rs485.mappings[i];
        if (!mapping.assigned) continue;
        if (mapping.capability_type == CAP_TEMP &&
            i >= LOGICAL_TEMP_SLOT_1 &&
            i <= LOGICAL_TEMP_SLOT_4 &&
            mapping.channel != (uint8_t)(i - LOGICAL_TEMP_SLOT_1)) {
            mapping.slave_uid = 0;
            mapping.slave_addr = 0;
            mapping.channel = 0;
            mapping.assigned = false;
            continue;
        }
        if (mapping.manual_override) {
            // Preserve manual choices while a known slave is temporarily
            // offline, but release mappings that reference a removed/replaced
            // slave or a capability that is no longer assigned.
            if (mapping_source_configured_locked(state, mapping)) continue;
            mapping.manual_override = false;
        } else if (mapping_source_usable_locked(state, mapping)) {
            continue;
        }

        mapping.slave_uid = 0;
        mapping.slave_addr = 0;
        mapping.channel = 0;
        mapping.assigned = false;
    }
}

static void auto_assign_temperature_locked(BuildingState& state) {
    for (uint8_t channel = 0; channel < DASHBOARD_TEMP_SLOTS; channel++) {
        LogicalMapping& mapping = state.rs485.mappings[LOGICAL_TEMP_SLOT_1 + channel];
        if (mapping.assigned || mapping.manual_override) continue;

        uint8_t count = state.rs485.slave_count;
        if (count > RS485_MAX_SLAVES) count = RS485_MAX_SLAVES;
        for (uint8_t i = 0; i < count; i++) {
            RS485SlaveState& slave = state.rs485.slaves[i];
            if (!slave_has_enabled_capability(slave, CAP_TEMP)) continue;
            if (!slave_temp_channel_enabled(slave, channel)) continue;
            if (temp_channel_already_mapped(state, slave.uid, slave.address, channel)) continue;
            assign_mapping(mapping, slave, channel);
            break;
        }
    }
}

static void auto_assign_single_locked(BuildingState& state, DashboardLogicalId logical_id, uint16_t capability) {
    LogicalMapping& mapping = state.rs485.mappings[logical_id];
    if (mapping.assigned || mapping.manual_override) return;

    uint8_t count = state.rs485.slave_count;
    if (count > RS485_MAX_SLAVES) count = RS485_MAX_SLAVES;
    for (uint8_t i = 0; i < count; i++) {
        RS485SlaveState& slave = state.rs485.slaves[i];
        if (!slave_has_enabled_capability(slave, capability)) continue;
        assign_mapping(mapping, slave, 0);
        return;
    }
}

static bool mapping_matches_slave(const LogicalMapping& mapping, const RS485SlaveState& slave) {
    if (!mapping.assigned) return false;
    return mapping.slave_uid != 0 ? mapping.slave_uid == slave.uid
                                  : mapping.slave_addr == slave.address;
}

static void auto_assign_room_lux_locked(BuildingState& state) {
    LogicalMapping& lux_mapping = state.rs485.mappings[LOGICAL_LUX_MAIN];
    RS485SlaveState* current_lux_slave =
        lux_mapping.assigned ? find_slave_by_mapping_locked(state, lux_mapping) : nullptr;
    if (current_lux_slave && is_ir_node(*current_lux_slave)) {
        lux_mapping.slave_uid = 0;
        lux_mapping.slave_addr = 0;
        lux_mapping.channel = 0;
        lux_mapping.assigned = false;
        lux_mapping.manual_override = false;
    }

    uint8_t count = state.rs485.slave_count;
    if (count > RS485_MAX_SLAVES) count = RS485_MAX_SLAVES;

    if (lux_mapping.assigned || lux_mapping.manual_override) return;

    for (uint8_t i = 0; i < count; i++) {
        RS485SlaveState& slave = state.rs485.slaves[i];
        if (!slave_has_enabled_capability(slave, CAP_LUX)) continue;
        if (is_ir_node(slave)) continue;
        assign_mapping(lux_mapping, slave, 0);
        return;
    }
}

static void compose_dashboard_locked(BuildingState& state) {
    DashboardModel next = {};
    for (uint8_t i = 0; i < DASHBOARD_TEMP_SLOTS; i++) {
        next.temp[i] = -100.0f;
        next.temp_valid[i] = false;
    }
    next.co2 = -1;
    next.lux = -1.0f;

    for (uint8_t slot = LOGICAL_TEMP_SLOT_1; slot <= LOGICAL_TEMP_SLOT_4; slot++) {
        const LogicalMapping& mapping = state.rs485.mappings[slot];
        if (!mapping.assigned) continue;
        RS485SlaveState* slave = find_slave_by_mapping_locked(state, mapping);
        uint8_t temp_slot = slot - LOGICAL_TEMP_SLOT_1;
        if (slave && slave_temp_channel_enabled(*slave, mapping.channel) &&
            mapping.channel < DASHBOARD_TEMP_SLOTS &&
            slave->temp_valid[mapping.channel]) {
            next.temp[temp_slot] = slave->temp[mapping.channel];
            next.temp_valid[temp_slot] = true;
        }
    }

    const LogicalMapping& co2_mapping = state.rs485.mappings[LOGICAL_CO2_MAIN];
    RS485SlaveState* co2_slave = co2_mapping.assigned ? find_slave_by_mapping_locked(state, co2_mapping) : nullptr;
    if (co2_slave && slave_has_enabled_capability(*co2_slave, CAP_CO2) && co2_slave->co2_valid) {
        next.co2 = co2_slave->co2;
        next.co2_valid = true;
    }

    float lux_sum = 0.0f;
    uint32_t lux_valid_count = 0;
    float sum_channels[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    uint32_t count_channels[4] = {0, 0, 0, 0};

    uint8_t slave_count = state.rs485.slave_count;
    if (slave_count > RS485_MAX_SLAVES) slave_count = RS485_MAX_SLAVES;
    for (uint8_t i = 0; i < slave_count; i++) {
        const RS485SlaveState& slave = state.rs485.slaves[i];
        if (slave.online && !is_ir_node(slave) &&
            slave_has_enabled_capability(slave, CAP_LUX) && slave.lux_valid) {
            lux_sum += slave.lux;
            lux_valid_count++;

            for (uint8_t ch = 0; ch < 4; ch++) {
                if (slave.lux_channel_valid[ch]) {
                    sum_channels[ch] += slave.lux_channel[ch];
                    count_channels[ch]++;
                }
            }
        }
    }

    if (lux_valid_count > 0) {
        next.lux = lux_sum / lux_valid_count;
        next.lux_valid = true;
        for (uint8_t ch = 0; ch < 4; ch++) {
            if (count_channels[ch] > 0) {
                next.lux_channel[ch] = sum_channels[ch] / count_channels[ch];
                next.lux_channel_valid[ch] = true;
            } else {
                next.lux_channel[ch] = 0.0f;
                next.lux_channel_valid[ch] = false;
            }
        }
    } else {
        next.lux = -1.0f;
        next.lux_valid = false;
        memset(next.lux_channel, 0, sizeof(next.lux_channel));
        memset(next.lux_channel_valid, 0, sizeof(next.lux_channel_valid));
    }
    state.sensor.lux = next.lux_valid ? next.lux : -1.0f; // Synchronize legacy state

    const LogicalMapping& human_mapping = state.rs485.mappings[LOGICAL_HUMAN_PRESENCE_MAIN];
    RS485SlaveState* human_slave = human_mapping.assigned ? find_slave_by_mapping_locked(state, human_mapping) : nullptr;
    if (human_slave && slave_has_enabled_capability(*human_slave, CAP_HUMAN_PRESENCE) &&
        human_slave->human_presence_valid) {
        next.human_presence = human_slave->human_presence;
        next.human_presence_valid = true;
    }

    const LogicalMapping& ac_mapping = state.rs485.mappings[LOGICAL_AC_CONTROL];
    RS485SlaveState* ac_slave = ac_mapping.assigned ? find_slave_by_mapping_locked(state, ac_mapping) : nullptr;
    next.ac_available = ac_slave && slave_has_enabled_capability(*ac_slave, CAP_AC_IR);

    const LogicalMapping& projector_mapping = state.rs485.mappings[LOGICAL_PROJECTOR_CONTROL];
    RS485SlaveState* projector_slave = projector_mapping.assigned ? find_slave_by_mapping_locked(state, projector_mapping) : nullptr;
    next.projector_available = projector_slave && slave_has_enabled_capability(*projector_slave, CAP_PROJECTOR_IR);

    if (memcmp(&state.rs485.dashboard, &next, sizeof(next)) != 0) {
        state.rs485.dashboard = next;
        state.ui_needs_update = true;
    }
}

void mapping_manager_update_locked(BuildingState& state) {
    clear_stale_auto_mappings_locked(state);
    auto_assign_temperature_locked(state);
    auto_assign_single_locked(state, LOGICAL_CO2_MAIN, CAP_CO2);
    auto_assign_single_locked(state, LOGICAL_HUMAN_PRESENCE_MAIN, CAP_HUMAN_PRESENCE);
    auto_assign_single_locked(state, LOGICAL_AC_CONTROL, CAP_AC_IR);
    auto_assign_single_locked(state, LOGICAL_PROJECTOR_CONTROL, CAP_PROJECTOR_IR);
    auto_assign_room_lux_locked(state);
    compose_dashboard_locked(state);
}
