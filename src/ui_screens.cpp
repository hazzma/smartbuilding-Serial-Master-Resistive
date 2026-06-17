#include "ui_screens.h"
#include "ui_widgets.h"
#include "ui_keyboard.h"
#include "rs485_manager.h"
#include "mapping_manager.h"
#include "mqtt_manager.h"
#include "time_manager.h"

#include <string.h>

void render_clock_setup(BuildingState& state);
void handle_clock_setup_touch(BuildingState& state, int tx, int ty);

static ScreenState current_screen = SCREEN_DASHBOARD;
static UIEventCallbacks ui_callbacks;

static int  editing_target = 0;
static char wifi_ssid[32]  = "";
static char wifi_pass[64]  = "";
static bool show_password  = false;

static bool dashboard_env_panel = false;
static bool dashboard_dragging = false;
static bool dashboard_moved = false;
static int  dashboard_drag_start_y = 0;
static char temp_lan_ip[16] = "192.168.1.177";
static char temp_lan_gw[16] = "192.168.1.1";
static char temp_lan_sn[16] = "255.255.255.0";
static char temp_lan_dns[16] = "8.8.8.8";
static bool lan_dhcp        = true;

static int  settings_page = 0;
static bool settings_dragging = false;
static bool settings_moved = false;
static int  settings_drag_start_x = 0;
static int  settings_drag_start_y = 0;

static bool clock_setup_manual_mode = false;
static int  manual_clock_year = 2026;
static int  manual_clock_month = 6;
static int  manual_clock_day = 17;
static int  manual_clock_hour = 19;
static int  manual_clock_minute = 47;

static float wifi_scan_scroll_y = 0.0f;
static float wifi_scan_scroll_target = 0.0f;
static bool  wifi_scan_dragging = false;
static bool  wifi_scan_moved = false;
static int   wifi_scan_drag_start_y = 0;
static int   wifi_scan_last_y = 0;
static uint32_t wifi_scan_last_interaction = 0;

static const int WIFI_SCAN_LIST_TOP = 72;
static const int WIFI_SCAN_LIST_BOTTOM = 270;
static const int WIFI_SCAN_ROW_H = 48;
static const int WIFI_SCAN_SCROLLBAR_X = 470;
static const int WIFI_SCAN_SCROLLBAR_W = 5;

static float slave_scroll_y = 0.0f;
static float slave_scroll_target = 0.0f;
static bool  slave_dragging = false;
static bool  slave_moved = false;
static int   slave_drag_start_y = 0;
static int   slave_last_y = 0;
static int   slave_selected_index = 0;
static int   slave_current_page = 0;
static int   slave_feature_page = 0;
static int   mapping_page = 0;
static int   mapping_selected_slot = 0;
static int   mapping_source_page = 0;
static bool  slave_detail_dummy = false;
static uint16_t slave_dummy_feature_mask = 0;

static const int SLAVE_LIST_TOP = 184;
static const int SLAVE_LIST_BOTTOM = 288;
static const int SLAVE_ROW_H = 34;
static const int SLAVE_PAGE_ROWS = 3;
static const int SLAVE_SCROLLBAR_X = 470;
static const int SLAVE_SCROLLBAR_W = 5;
static const uint8_t SLAVE_PLACEHOLDER_INDEX = 0xFF;
static const int SLAVE_FEATURE_PAGE_ROWS = 3;

void screens_init(UIEventCallbacks callbacks) {
    ui_callbacks = callbacks;
    widgets_init();
}

void screens_set(ScreenState s) {
    if (current_screen == s) return;
    Serial.printf("[UI] Screen: %d -> %d\n", current_screen, s);
    current_screen = s;
    data_lock(g_state);
    g_state.ui_needs_update = true;
    data_unlock(g_state);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

bool isHit(int tx, int ty, int x, int y, int w, int h) {
    return (tx >= x && tx <= x + w && ty >= y && ty <= y + h);
}

static int wifi_scan_view_h() {
    return WIFI_SCAN_LIST_BOTTOM - WIFI_SCAN_LIST_TOP;
}

static int wifi_scan_content_h(uint8_t count) {
    return count * WIFI_SCAN_ROW_H;
}

static int wifi_scan_max_scroll(uint8_t count) {
    int max_scroll = wifi_scan_content_h(count) - wifi_scan_view_h();
    return max_scroll > 0 ? max_scroll : 0;
}

static void wifi_scan_clamp_scroll(uint8_t count) {
    int max_scroll = wifi_scan_max_scroll(count);
    if (wifi_scan_scroll_target < 0) wifi_scan_scroll_target = 0;
    if (wifi_scan_scroll_target > max_scroll) wifi_scan_scroll_target = max_scroll;
    if (wifi_scan_scroll_y < 0) wifi_scan_scroll_y = 0;
    if (wifi_scan_scroll_y > max_scroll) wifi_scan_scroll_y = max_scroll;
}


static int slave_list_view_h() {
    return SLAVE_LIST_BOTTOM - SLAVE_LIST_TOP;
}

static int slave_list_content_h(uint8_t count) {
    return count * SLAVE_ROW_H;
}

static int slave_list_max_scroll(uint8_t count) {
    int max_scroll = slave_list_content_h(count) - slave_list_view_h();
    return max_scroll > 0 ? max_scroll : 0;
}

static void slave_list_clamp_scroll(uint8_t count) {
    int max_scroll = slave_list_max_scroll(count);
    if (slave_scroll_target < 0) slave_scroll_target = 0;
    if (slave_scroll_target > max_scroll) slave_scroll_target = max_scroll;
    if (slave_scroll_y < 0) slave_scroll_y = 0;
    if (slave_scroll_y > max_scroll) slave_scroll_y = max_scroll;
    if (slave_selected_index < 0) slave_selected_index = 0;
    if (count == 0) slave_selected_index = 0;
    else if (slave_selected_index >= count) slave_selected_index = count - 1;
}

static const char* rs485_ui_result_name(uint8_t result) {
    switch ((RS485RxResult)result) {
        case RS485_RX_OK: return "OK";
        case RS485_RX_TIMEOUT: return "TIMEOUT";
        case RS485_RX_CRC_ERROR: return "CRC";
        case RS485_RX_SEQ_MISMATCH: return "SEQ";
        case RS485_RX_CMD_MISMATCH: return "CMD";
        case RS485_RX_ADDR_MISMATCH: return "ADDR";
        case RS485_RX_NACK: return "NACK";
        case RS485_RX_ERROR: return "ERROR";
        default: return "-";
    }
}

static float dashboard_avg_temp(const SensorData& sensor, uint8_t* valid_count = nullptr) {
    float sum = 0.0f;
    uint8_t count = 0;
    for (uint8_t i = 0; i < 4; i++) {
        if (!sensor.sensor_error[i] && sensor.temp[i] > -50.0f) {
            sum += sensor.temp[i];
            count++;
        }
    }
    if (valid_count) *valid_count = count;
    return count > 0 ? sum / count : -100.0f;
}

static float dashboard_model_avg_temp(const DashboardModel& dashboard, uint8_t* valid_count = nullptr) {
    float sum = 0.0f;
    uint8_t count = 0;
    for (uint8_t i = 0; i < DASHBOARD_TEMP_SLOTS; i++) {
        if (dashboard.temp_valid[i]) {
            sum += dashboard.temp[i];
            count++;
        }
    }
    if (valid_count) *valid_count = count;
    return count > 0 ? sum / count : -100.0f;
}

static bool rs485_has_enabled_capability(const RS485State& rs485, uint16_t capability) {
    uint8_t count = rs485.slave_count;
    if (count > RS485_MAX_SLAVES) count = RS485_MAX_SLAVES;
    for (uint8_t i = 0; i < count; i++) {
        if (rs485.slaves[i].online && rs485.slaves[i].capability_synced &&
            (rs485.slaves[i].capability & capability) &&
            (rs485.slaves[i].enabled_mask & capability)) {
            return true;
        }
    }
    return false;
}

struct UiRect {
    int x;
    int y;
    int w;
    int h;
};

enum DashboardLayoutMode {
    DASH_LAYOUT_TEMP_CENTER_LARGE,
    DASH_LAYOUT_TEMP_COMPACT_WITH_CONTROLS,
    DASH_LAYOUT_STATUS_EMPTY,
    DASH_LAYOUT_SINGLE_CONTROL_CENTER,
    DASH_LAYOUT_MULTI_CONTROL_SPLIT
};

struct DashboardUiModel {
    bool has_temp;
    float avg_temp;
    bool has_ac;
    bool has_projector;
    bool has_led;
    bool has_co2;
    int co2;
    bool wifi_connected;
    bool lan_connected;
    bool bus_ok;
    bool slave_online;
    float ac_target_temp;
    bool ac_on;
    uint8_t ac_fan_speed;
    uint8_t ac_swing_mode;
    bool projector_on;
    bool led_on;
    bool led_channel_on[2];
    uint8_t led_channel_count;
    bool ac_mirrors;
    DashboardLayoutMode layout;
    uint8_t proj_verif_state;
    bool proj_hw_fail;
};

static bool rs485_has_online_slave(const RS485State& rs485) {
    uint8_t count = rs485.slave_count;
    if (count > RS485_MAX_SLAVES) count = RS485_MAX_SLAVES;
    for (uint8_t i = 0; i < count; i++) {
        if (rs485.slaves[i].online) return true;
    }
    return false;
}

static bool rs485_has_ir_combo_node(const RS485State& rs485) {
    uint8_t count = rs485.slave_count;
    if (count > RS485_MAX_SLAVES) count = RS485_MAX_SLAVES;
    for (uint8_t i = 0; i < count; i++) {
        const RS485SlaveState& slave = rs485.slaves[i];
        if (slave.profile == IR_COMBO_NODE &&
            (slave.enabled_mask & RS485_CAP_AC_IR) &&
            (slave.enabled_mask & RS485_CAP_PROJECTOR_IR)) {
            return true;
        }
    }
    return false;
}

static DashboardLayoutMode dashboard_choose_layout(bool has_temp, bool has_ac,
                                                   bool has_projector, bool has_led) {
    if (has_temp) {
        if (!has_ac && !has_projector && !has_led) return DASH_LAYOUT_TEMP_CENTER_LARGE;
        return DASH_LAYOUT_TEMP_COMPACT_WITH_CONTROLS;
    }

    int controls = (has_ac ? 1 : 0) + (has_projector ? 1 : 0) + (has_led ? 1 : 0);
    if (controls == 0) return DASH_LAYOUT_STATUS_EMPTY;
    if (controls == 1) return DASH_LAYOUT_SINGLE_CONTROL_CENTER;
    return DASH_LAYOUT_MULTI_CONTROL_SPLIT;
}

static DashboardUiModel dashboard_make_ui_model(const BuildingState& state) {
    DashboardUiModel model = {};
    uint8_t valid_temp_count = 0;
    model.avg_temp = dashboard_model_avg_temp(state.rs485.dashboard, &valid_temp_count);
    model.has_temp = valid_temp_count > 0;
    model.has_ac = state.rs485.dashboard.ac_available;
    model.has_projector = state.rs485.dashboard.projector_available;
    model.has_led = rs485_has_enabled_capability(state.rs485, RS485_CAP_LIGHT_RELAY);
    model.has_co2 = state.rs485.dashboard.co2_valid;
    model.co2 = state.rs485.dashboard.co2;
    model.wifi_connected = state.net.wifi_connected;
    model.lan_connected = state.net.lan_connected;
    model.bus_ok = state.rs485.bus_ok;
    model.slave_online = rs485_has_online_slave(state.rs485);
    model.ac_target_temp = state.sensor.temp_target;
    model.ac_on = state.sensor.ac_on;
    model.ac_fan_speed = state.sensor.ac_fan_speed;
    model.ac_swing_mode = state.sensor.ac_swing_mode;
    model.projector_on = state.sensor.projector_on;
    model.led_on = state.sensor.light_on;
    uint8_t slave_count = state.rs485.slave_count;
    if (slave_count > RS485_MAX_SLAVES) slave_count = RS485_MAX_SLAVES;
    for (uint8_t i = 0; i < slave_count; i++) {
        const RS485SlaveState& slave = state.rs485.slaves[i];
        if (!slave.online || !(slave.enabled_mask & RS485_CAP_LIGHT_RELAY)) continue;
        model.led_channel_count = slave.relay_count == 0 ? 1 : slave.relay_count;
        if (model.led_channel_count > 2) model.led_channel_count = 2;
        for (uint8_t channel = 0; channel < model.led_channel_count; channel++) {
            model.led_channel_on[channel] = slave.relay_state[channel] != 0;
        }
        break;
    }
    model.ac_mirrors = rs485_has_ir_combo_node(state.rs485);
    model.layout = dashboard_choose_layout(model.has_temp, model.has_ac,
                                           model.has_projector, model.has_led);
    model.proj_verif_state = state.sensor.proj_verif_state;
    model.proj_hw_fail = state.sensor.proj_hardware_failed;
    return model;
}

static bool dashboard_is_full_control_layout(const DashboardUiModel& model) {
    return model.has_temp && model.has_ac && model.has_projector && model.has_led;
}

static void dashboard_draw_transparent_temp(float temp) {
    char value[20];
    snprintf(value, sizeof(value), "%.1f C", temp);
    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextFont(4);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString(value, 240, 64);
    p_canvas->setTextDatum(TextDatum::TopLeft);
}

static void dashboard_draw_led_widget(int x, int y, int w, int h, const DashboardUiModel& model) {
    if (model.led_channel_count > 1) {
        int gap = 8;
        int channel_w = (w - gap) / 2;
        drawLargeControlButton(x, y, channel_w, h, "LED 1", model.led_channel_on[0]);
        drawLargeControlButton(x + channel_w + gap, y, w - channel_w - gap, h,
                               "LED 2", model.led_channel_on[1]);
    } else {
        drawLargeControlButton(x, y, w, h, "LED 1", model.led_channel_on[0]);
    }
}

static void dashboard_draw_ac_widget(int x, int y, int w, int h, const DashboardUiModel& model) {
    drawAcTargetWidget(x, y, w, h, model.ac_target_temp, model.ac_on);
    if (h < 180) return;

    static const char* SWING_MODES[] = {"FIX", "AUTO", "UP", "MID+", "MID", "MID-", "DOWN", "NEXT", "PREV", "COMF", "PWR"};
    static const char* FAN_MODES[] = {"AUTO", "LOW", "MID", "HIGH", "QUIET", "TURBO"};
    int button_y = y + h - 62;
    int button_w = (w - 30) / 2;

    drawCardBase(x + 10, button_y, button_w, 52, COLOR_CARD_BG);
    drawCardBase(x + 20 + button_w, button_y, button_w, 52, COLOR_CARD_BG);

    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextFont(1);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString("SWING", x + 10 + button_w / 2, button_y + 14);
    p_canvas->drawString("FAN", x + 20 + button_w + button_w / 2, button_y + 14);

    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    const char* swing_label = model.ac_swing_mode < (sizeof(SWING_MODES) / sizeof(SWING_MODES[0]))
                                  ? SWING_MODES[model.ac_swing_mode]
                                  : "--";
    const char* fan_label = model.ac_fan_speed < (sizeof(FAN_MODES) / sizeof(FAN_MODES[0]))
                                ? FAN_MODES[model.ac_fan_speed]
                                : "--";
    p_canvas->drawString(swing_label, x + 10 + button_w / 2, button_y + 36);
    p_canvas->drawString(fan_label, x + 20 + button_w + button_w / 2, button_y + 36);
    p_canvas->setTextDatum(TextDatum::TopLeft);
}

static UiRect dashboard_menu_rect() {
    return {18, 264, 112, 44};
}

static bool hit_rect(int tx, int ty, const UiRect& r) {
    return isHit(tx, ty, r.x, r.y, r.w, r.h);
}

static void dashboard_draw_menu_button() {
    UiRect r = dashboard_menu_rect();
    drawCardBase(r.x, r.y, r.w, r.h, COLOR_CARD_BG);
    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString("MENU", r.x + r.w / 2, r.y + r.h / 2);
    p_canvas->setTextDatum(TextDatum::TopLeft);
}

static UiRect dashboard_env_close_rect() {
    return {366, 56, 84, 38};
}

static void render_dashboard_environment_panel(const BuildingState& state) {
    drawCardBase(20, 54, 440, 234, COLOR_CARD_BG);

    p_canvas->setTextDatum(TextDatum::TopLeft);
    p_canvas->setTextFont(4);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString("Environment", 38, 68);

    UiRect close = dashboard_env_close_rect();
    drawCardBase(close.x, close.y, close.w, close.h, COLOR_STAT_OFF);
    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString("CLOSE", close.x + close.w / 2, close.y + close.h / 2);

    p_canvas->setTextDatum(TextDatum::TopLeft);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString("Lux", 48, 126);
    p_canvas->drawString("Human Presence", 48, 196);

    char lux_buf[32];
    if (state.rs485.dashboard.lux_valid) snprintf(lux_buf, sizeof(lux_buf), "%.0f lx", state.rs485.dashboard.lux);
    else snprintf(lux_buf, sizeof(lux_buf), "--");

    const char* presence = "Unknown";
    if (state.rs485.dashboard.human_presence_valid) {
        presence = state.rs485.dashboard.human_presence ? "Detected" : "Empty";
    }

    p_canvas->setTextDatum(TextDatum::MiddleRight);
    p_canvas->setTextFont(4);
    p_canvas->setTextColor(state.rs485.dashboard.lux_valid ? COLOR_TEXT_MAIN : COLOR_TEXT_SEC);
    p_canvas->drawString(lux_buf, 430, 140);

    p_canvas->setTextColor(state.rs485.dashboard.human_presence_valid ? COLOR_TEXT_MAIN : COLOR_TEXT_SEC);
    p_canvas->drawString(presence, 430, 210);

    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString("Swipe down to close", 240, 264);
    p_canvas->setTextDatum(TextDatum::TopLeft);
}

// ── Screen Renderers ──────────────────────────────────────────────────────────

void render_dashboard_legacy(BuildingState& state, int fps) {
    p_canvas->fillScreen(COLOR_BG_MAIN);

    drawNotifBar(state.net.wifi_connected, state.net.lan_connected,
                 state.net.mqtt_ok, state.net.conn_status,
                 state.net.room_name, state.net.time_str);

    if (state.dashboard_page == 0) {
        // ── Left Panel: AC Control (x:10, y:40, w:220, h:255) ──
        drawCardBase(10, 40, 220, 255, COLOR_CARD_BG);

        p_canvas->setTextColor(COLOR_TEXT_SEC);
        p_canvas->setTextFont(2);
        p_canvas->drawString("AC CONTROL", 25, 52);

        p_canvas->setTextColor(COLOR_ACCENT_MAIN);
        p_canvas->setTextFont(6);
        char target_buf[16];
        snprintf(target_buf, sizeof(target_buf), "%.1f", state.sensor.temp_target);
        p_canvas->setTextDatum(TextDatum::MiddleCenter);
        p_canvas->drawString(target_buf, 115, 105);

        p_canvas->setTextFont(2);
        p_canvas->setTextColor(COLOR_TEXT_SEC);
        p_canvas->drawString("Target Temp (C)", 115, 75);
        p_canvas->setTextDatum(TextDatum::TopLeft);

        // UP / DOWN buttons
        drawUpDownButton(20, 145, 95, 55, "UP",   true);
        drawUpDownButton(125, 145, 95, 55, "DOWN", false);

        // AC Toggle: x:20, y:210, w:200, h:50
        drawToggleButton(20, 210, 200, 50, "AC POWER", state.sensor.ac_on);

        // ── Right Panel: Sensor Grid ──
        // Temp cards (4): each 110x72
        drawTempCard(240, 40,  110, 72, "Suhu 1", state.sensor.temp[0], state.sensor.sensor_error[0]);
        drawTempCard(362, 40,  110, 72, "Suhu 2", state.sensor.temp[1], state.sensor.sensor_error[1]);
        drawTempCard(240, 120, 110, 72, "Suhu 3", state.sensor.temp[2], state.sensor.sensor_error[2]);
        drawTempCard(362, 120, 110, 72, "Suhu 4", state.sensor.temp[3], state.sensor.sensor_error[3]);

        // Projector: x:240, y:203, w:110, h:47
        drawToggleButton(240, 203, 110, 47, "Projector", state.sensor.projector_on);
        // LED CTL:   x:362, y:203, w:110, h:47
        drawToggleButton(362, 203, 110, 47, "LED CTL",   state.sensor.light_on);

    } else {
        drawCO2Card(10,  45, 225, 120, state.sensor.co2);
        drawLuxCard(245, 45, 225, 120, state.sensor.lux);
        drawPresenceBadge(10, 175, 225, 110, state.sensor.human_presence);

        float sum = 0; int cnt = 0;
        for (int i = 0; i < 4; i++) {
            if (!state.sensor.sensor_error[i] && state.sensor.temp[i] > -50.0f) {
                sum += state.sensor.temp[i]; cnt++;
            }
        }
        float avg = cnt > 0 ? sum / cnt : -100.0f;
        drawTempCard(245, 175, 225, 110, "AVG TEMP", avg, avg < -50.0f);
    }

    // ── Footer: Page dots + PAGE button ──
    p_canvas->fillCircle(225, 308, 5, state.dashboard_page == 0 ? COLOR_ACCENT_MAIN : COLOR_CARD_BG);
    p_canvas->fillCircle(245, 308, 5, state.dashboard_page == 1 ? COLOR_ACCENT_MAIN : COLOR_CARD_BG);

    drawCardBase(370, 290, 100, 28, COLOR_CARD_BG);
    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->setTextFont(2);
    p_canvas->drawString("PAGE >>", 420, 304);
    p_canvas->setTextDatum(TextDatum::TopLeft);
}

void render_dashboard(BuildingState& state, int fps) {
    (void)fps;
    drawWallpaperBackground();

    DashboardUiModel model = dashboard_make_ui_model(state);
    bool empty_hero = model.layout == DASH_LAYOUT_STATUS_EMPTY;
    drawDashboardTopBar(state.net.time_str, model.wifi_connected, model.lan_connected,
                        model.bus_ok, model.slave_online, !empty_hero);

    if (model.has_co2) {
        drawCO2Chip(198, 10, model.co2);
    }

    const char* proj_sub = nullptr;
    if (model.proj_hw_fail) {
        proj_sub = "CHK PROJ";
    } else if (model.proj_verif_state == 1 || model.proj_verif_state == 3) {
        proj_sub = "POWERING";
    } else if (model.proj_verif_state == 4) {
        proj_sub = "NO LUX";
    } else if (model.proj_verif_state == 5) {
        proj_sub = "CHK LUX";
    } else if (model.proj_verif_state == 6) {
        proj_sub = "CHK PROJ";
    }

    switch (model.layout) {
        case DASH_LAYOUT_TEMP_CENTER_LARGE:
            drawLargeTempWidget(40, 78, 400, 174, model.avg_temp, true, true);
            break;

        case DASH_LAYOUT_TEMP_COMPACT_WITH_CONTROLS: {
            if (dashboard_is_full_control_layout(model)) {
                dashboard_draw_transparent_temp(model.avg_temp);
                dashboard_draw_ac_widget(18, 88, 224, 210, model);
                drawLargeControlButton(258, 88, 204, 94, "Projector", model.projector_on, proj_sub);
                dashboard_draw_led_widget(258, 194, 204, 104, model);
            } else if (model.has_ac && !model.has_projector && !model.has_led) {
                drawLargeTempWidget(24, 104, 204, 124, model.avg_temp, true, false);
                dashboard_draw_ac_widget(252, 104, 204, 124, model);
            } else if (model.has_ac && model.has_projector && !model.has_led) {
                dashboard_draw_ac_widget(18, 64, 216, 234, model);
                drawLargeControlButton(306, 64, 136, 136, "Projector", model.projector_on, proj_sub);
                drawLargeTempWidget(252, 214, 216, 84, model.avg_temp, true, false);
            } else {
                if (model.has_ac) {
                    drawLargeTempWidget(24, 72, 204, 104, model.avg_temp, true, false);
                    dashboard_draw_ac_widget(252, 72, 204, 104, model);
                } else {
                    drawLargeTempWidget(88, 72, 304, 104, model.avg_temp, true, false);
                }

                if (model.has_projector && model.has_led) {
                    drawLargeControlButton(24, 190, 204, 104, "Projector", model.projector_on, proj_sub);
                    dashboard_draw_led_widget(252, 190, 204, 104, model);
                } else if (model.has_projector) {
                    drawLargeControlButton(88, 190, 304, 104, "Projector", model.projector_on, proj_sub);
                } else if (model.has_led) {
                    dashboard_draw_led_widget(88, 190, 304, 104, model);
                }
            }

            if (!model.has_projector && !model.has_led && !model.has_ac) {
                drawDashboardEmptyState("No Control Active", "Open Menu > Slave Manager");
            }
            break;
        }

        case DASH_LAYOUT_STATUS_EMPTY:
            drawDashboardEmptyHero(state.net.time_str, "No Device Active", "Open Menu > Slave Manager");
            break;

        case DASH_LAYOUT_SINGLE_CONTROL_CENTER:
            if (model.has_ac) {
                dashboard_draw_ac_widget(98, 88, 284, 150, model);
            } else if (model.has_projector) {
                drawLargeControlButton(72, 92, 336, 144, "Projector", model.projector_on, proj_sub);
            } else if (model.has_led) {
                dashboard_draw_led_widget(72, 92, 336, 144, model);
            }
            break;

        case DASH_LAYOUT_MULTI_CONTROL_SPLIT:
            if (model.has_ac && model.has_projector && !model.has_led) {
                dashboard_draw_ac_widget(18, 62, 224, 236, model);
                drawLargeControlButton(258, 75, 210, 210, "Projector", model.projector_on, proj_sub);
            } else if (model.has_ac) {
                dashboard_draw_ac_widget(24, 76, 206, 142, model);
                if (model.has_projector) {
                    drawLargeControlButton(250, 76, 206, 74, "Projector", model.projector_on, proj_sub);
                }
                if (model.has_led) {
                    dashboard_draw_led_widget(250, 158, 206, 74, model);
                }
            } else {
                drawLargeControlButton(24, 92, 204, 144, "Projector", model.projector_on, proj_sub);
                dashboard_draw_led_widget(252, 92, 204, 144, model);
            }
            break;
    }

    if (dashboard_env_panel) {
        render_dashboard_environment_panel(state);
    }
}

void render_temperature_detail(BuildingState& state) {
    p_canvas->fillScreen(COLOR_BG_MAIN);

    uint8_t valid_count = 0;
    const DashboardModel& dashboard = state.rs485.dashboard;
    float avg = dashboard_model_avg_temp(dashboard, &valid_count);

    p_canvas->setTextDatum(TextDatum::TopLeft);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->setTextFont(4);
    p_canvas->drawString("Temperature", 20, 10);
    p_canvas->drawFastHLine(0, 46, 480, COLOR_ACCENT_MAIN);

    drawCardBase(372, 8, 90, 34, COLOR_STAT_OFF);
    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString("BACK", 417, 25);

    drawCardBase(20, 60, 440, 74, COLOR_CARD_BG);
    char avg_buf[40];
    if (valid_count == 0) snprintf(avg_buf, sizeof(avg_buf), "--");
    else snprintf(avg_buf, sizeof(avg_buf), "%.1f C", avg);
    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextFont(6);
    p_canvas->setTextColor(valid_count == 0 ? COLOR_TEXT_SEC : COLOR_ACCENT_MAIN);
    p_canvas->drawString(avg_buf, 240, 92);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString("Average Room Temp", 240, 124);

    for (uint8_t i = 0; i < 4; i++) {
        int y = 146 + i * 40;
        drawCardBase(20, y, 440, 34, COLOR_CARD_BG);
        char label[24];
        snprintf(label, sizeof(label), "Point %u", i + 1);
        p_canvas->setTextDatum(TextDatum::MiddleLeft);
        p_canvas->setTextFont(2);
        p_canvas->setTextColor(COLOR_TEXT_SEC);
        p_canvas->drawString(label, 36, y + 17);

        char value[24];
        bool invalid = !dashboard.temp_valid[i];
        if (invalid) snprintf(value, sizeof(value), "--");
        else snprintf(value, sizeof(value), "%.1f C", dashboard.temp[i]);
        p_canvas->setTextColor(invalid ? COLOR_TEXT_SEC : COLOR_TEXT_MAIN);
        p_canvas->setTextDatum(TextDatum::MiddleCenter);
        p_canvas->setTextFont(4);
        p_canvas->drawString(value, 222, y + 17);

        p_canvas->setTextDatum(TextDatum::MiddleRight);
        p_canvas->setTextFont(2);
        p_canvas->setTextColor(invalid ? COLOR_STAT_WARN : COLOR_STAT_ON);
        p_canvas->drawString(invalid ? "Not assigned" : "Live", 440, y + 17);
    }

    p_canvas->setTextDatum(TextDatum::TopLeft);
}

void render_settings(BuildingState& state) {
    drawWallpaperBackground();

    p_canvas->setTextDatum(TextDatum::TopLeft);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->setTextFont(4);
    p_canvas->drawString(settings_page == 0 ? "Settings" : "Settings 2", 20, 10);

    drawCardBase(338, 8, 122, 38, COLOR_STAT_OFF);
    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString("BACK", 399, 27);

    struct SettingsTile {
        int x;
        int y;
        int w;
        int h;
        const char* title;
        const char* subtitle;
        uint16_t accent;
    };

    if (settings_page == 0) {
        // --- PAGE 1 ---
        // Network Priority Toggle
        drawCardBase(20, 58, 440, 48, COLOR_CARD_BG);
        p_canvas->setTextDatum(TextDatum::MiddleLeft);
        p_canvas->setTextFont(2);
        p_canvas->setTextColor(COLOR_TEXT_SEC);
        p_canvas->drawString("Network Priority", 38, 82);
        p_canvas->setTextDatum(TextDatum::MiddleRight);
        p_canvas->setTextFont(4);
        p_canvas->setTextColor(COLOR_TEXT_MAIN);
        p_canvas->drawString(state.net.net_priority == 0 ? "WiFi" : "LAN", 442, 82);

        // WiFi Setup, LAN Setup, Slave Manager/Slaves
        SettingsTile tiles[3] = {
            {20, 126, 136, 92, "WiFi", state.net.wifi_connected ? "Connected" : "Setup", (uint16_t)(state.net.wifi_connected ? COLOR_STAT_ON : COLOR_STAT_WARN)},
            {172, 126, 136, 92, "LAN", state.net.lan_connected ? "Connected" : "Setup", (uint16_t)(state.net.lan_connected ? COLOR_STAT_ON : COLOR_STAT_WARN)},
            {324, 126, 136, 92, "Slaves", state.rs485.bus_ok ? "Fieldbus" : "RS485", (uint16_t)(state.rs485.bus_ok ? COLOR_STAT_ON : COLOR_STAT_ERR)}
        };

        for (int i = 0; i < 3; i++) {
            const SettingsTile& tile = tiles[i];
            drawCardBase(tile.x, tile.y, tile.w, tile.h, COLOR_CARD_BG);
            p_canvas->fillRoundRect(tile.x + 14, tile.y + 14, 8, tile.h - 28, 4, tile.accent);

            p_canvas->setTextDatum(TextDatum::MiddleLeft);
            p_canvas->setTextFont(4);
            p_canvas->setTextColor(COLOR_TEXT_MAIN);
            p_canvas->drawString(tile.title, tile.x + 34, tile.y + 40);

            p_canvas->setTextFont(2);
            p_canvas->setTextColor(COLOR_TEXT_SEC);
            p_canvas->drawString(tile.subtitle, tile.x + 34, tile.y + 66);
        }

        // RS485 full-width card
        drawCardBase(20, 238, 440, 54, COLOR_CARD_BG);
        p_canvas->setTextColor(COLOR_TEXT_SEC);
        p_canvas->setTextFont(2);
        p_canvas->setTextDatum(TextDatum::MiddleLeft);
        p_canvas->drawString("RS485", 38, 260);

        p_canvas->setTextDatum(TextDatum::MiddleRight);
        p_canvas->setTextColor(state.rs485.bus_ok ? COLOR_STAT_ON : COLOR_STAT_ERR);
        p_canvas->drawString(state.rs485.status, 442, 260);

        p_canvas->setTextDatum(TextDatum::MiddleLeft);
        p_canvas->setTextColor(COLOR_TEXT_SEC);
        p_canvas->drawString("Swipe left for MQTT & Device Info / Tap Priority to switch network", 38, 280);

    } else {
        // --- PAGE 2 ---
        struct SettingsItem {
            const char* title;
            const char* subtitle;
            uint16_t accent;
        };

        SettingsItem items[3] = {
            {"MQTT Setup", state.net.mqtt_ok ? "Broker Connected" : "MQTT Setup", (uint16_t)(state.net.mqtt_ok ? COLOR_STAT_ON : COLOR_STAT_WARN)},
            {"Device Info", "Name / Class Room", COLOR_STAT_ON},
            {"Clock Setup", state.net.use_manual_time ? "Manual Mode" : "NTP Mode", COLOR_STAT_ON}
        };

        const int row_y[3] = {58, 126, 194};
        for (int i = 0; i < 3; i++) {
            drawCardBase(20, row_y[i], 440, 58, COLOR_CARD_BG);
            p_canvas->fillRoundRect(34, row_y[i] + 8, 6, 42, 3, items[i].accent);

            p_canvas->setTextDatum(TextDatum::MiddleLeft);
            p_canvas->setTextFont(4);
            p_canvas->setTextColor(COLOR_TEXT_MAIN);
            p_canvas->drawString(items[i].title, 52, row_y[i] + 29);

            p_canvas->setTextDatum(TextDatum::MiddleRight);
            p_canvas->setTextFont(2);
            p_canvas->setTextColor(COLOR_TEXT_SEC);
            p_canvas->drawString(items[i].subtitle, 430, row_y[i] + 29);
        }

        drawCardBase(20, 262, 440, 38, COLOR_CARD_BG);
        p_canvas->setTextDatum(TextDatum::MiddleLeft);
        p_canvas->setTextFont(2);
        p_canvas->setTextColor(COLOR_TEXT_SEC);
        p_canvas->drawString("Swipe right to return / Tap MQTT, Device Info, or Clock", 38, 281);

        p_canvas->setTextDatum(TextDatum::MiddleRight);
        p_canvas->setTextColor(state.net.mqtt_ok ? COLOR_STAT_ON : COLOR_STAT_WARN);
        p_canvas->drawString(state.net.mqtt_ok ? "MQTT OK" : "MQTT offline", 442, 281);
    }

    // Page Dots
    p_canvas->fillCircle(230, 308, 5, settings_page == 0 ? COLOR_ACCENT_MAIN : COLOR_CARD_BG);
    p_canvas->fillCircle(250, 308, 5, settings_page == 1 ? COLOR_ACCENT_MAIN : COLOR_CARD_BG);
}

void render_device_info(BuildingState& state) {
    drawWallpaperBackground();

    p_canvas->setTextDatum(TextDatum::TopLeft);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->setTextFont(4);
    p_canvas->drawString("Device Info", 20, 10);

    drawCardBase(338, 8, 122, 38, COLOR_STAT_OFF);
    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString("BACK", 399, 27);

    drawCardBase(20, 58, 440, 48, COLOR_CARD_BG);
    p_canvas->setTextDatum(TextDatum::MiddleLeft);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString("Device Name", 38, 74);
    p_canvas->setTextFont(4);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString(state.net.device_name[0] ? state.net.device_name : "Meeting Room Master", 38, 94);

    drawCardBase(20, 118, 210, 54, COLOR_CARD_BG);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString("Class Name", 38, 136);
    p_canvas->setTextFont(4);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString(state.net.class_name[0] ? state.net.class_name : "HD01", 38, 156);

    drawCardBase(250, 118, 210, 54, COLOR_CARD_BG);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString("Firmware", 268, 136);
    p_canvas->setTextFont(4);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString("Firmware V2", 268, 156);

    drawCardBase(20, 184, 440, 48, COLOR_CARD_BG);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString("Firmware By Hansel Kay CE LAB", 38, 204);

    char preview[64];
    snprintf(preview, sizeof(preview), "Topic: Class %s co2", state.net.class_name[0] ? state.net.class_name : "HD01");
    drawCardBase(20, 244, 440, 54, COLOR_CARD_BG);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString(preview, 38, 262);
    p_canvas->setTextColor(state.net.mqtt_ok ? COLOR_STAT_ON : COLOR_STAT_WARN);
    p_canvas->drawString(state.net.mqtt_ok ? "MQTT OK" : "MQTT offline", 38, 282);

    p_canvas->setTextDatum(TextDatum::TopLeft);
}

void render_mqtt_setup(BuildingState& state) {
    drawWallpaperBackground();

    p_canvas->setTextDatum(TextDatum::TopLeft);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->setTextFont(4);
    p_canvas->drawString("MQTT Setup", 20, 10);

    drawCardBase(350, 8, 110, 38, COLOR_STAT_OFF);
    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString("BACK", 405, 27);

    drawCardBase(20, 54, 440, 48, COLOR_CARD_BG);
    p_canvas->setTextDatum(TextDatum::TopLeft);
    p_canvas->setTextFont(1);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString("Broker / Host (tap to edit)", 36, 62);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString(state.net.mqtt_server[0] ? state.net.mqtt_server : "-", 36, 82);

    drawCardBase(20, 110, 210, 48, COLOR_CARD_BG);
    drawCardBase(250, 110, 210, 48, state.net.mqtt_use_tls ? COLOR_STAT_ON : COLOR_CARD_BG);
    char port_text[24];
    snprintf(port_text, sizeof(port_text), "Port: %u", state.net.mqtt_port);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString(port_text, 36, 138);
    p_canvas->drawString(state.net.mqtt_use_tls ? "TLS: ON" : "TLS: OFF", 266, 138);

    drawCardBase(20, 166, 440, 44, COLOR_CARD_BG);
    p_canvas->setTextFont(1);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString("Username (tap to edit)", 36, 172);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString(state.net.mqtt_user[0] ? state.net.mqtt_user : "(empty)", 36, 194);

    drawCardBase(20, 218, 440, 44, COLOR_CARD_BG);
    p_canvas->setTextFont(1);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString("Password (tap to edit)", 36, 224);
    char masked[40] = "";
    size_t pass_len = strlen(state.net.mqtt_pass);
    if (pass_len > sizeof(masked) - 1) pass_len = sizeof(masked) - 1;
    for (size_t i = 0; i < pass_len; i++) masked[i] = '*';
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString(masked[0] ? masked : "(empty)", 36, 246);

    drawCardBase(20, 274, 210, 38, COLOR_CARD_BG);
    drawCardBase(250, 274, 210, 38, COLOR_STAT_ON);
    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(state.net.mqtt_ok ? COLOR_STAT_ON : COLOR_STAT_WARN);
    p_canvas->drawString(state.net.mqtt_ok ? "MQTT CONNECTED" : "MQTT OFFLINE", 125, 293);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString("SAVE & RECONNECT", 355, 293);
    p_canvas->setTextDatum(TextDatum::TopLeft);
}

void render_wifi_config(BuildingState& state) {
    drawWallpaperBackground();
    p_canvas->setTextDatum(TextDatum::TopLeft);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->setTextFont(4);
    p_canvas->drawString("WiFi Setup", 20, 10);

    drawCardBase(350, 8, 110, 38, COLOR_STAT_OFF);
    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString("BACK", 405, 27);

    drawCardBase(20, 58, 440, 62, COLOR_CARD_BG);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->setTextDatum(TextDatum::TopLeft);
    p_canvas->drawString("SSID", 38, 68);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->setTextDatum(TextDatum::MiddleLeft);
    p_canvas->setTextFont(4);
    p_canvas->drawString(wifi_ssid[0] ? wifi_ssid : "Tap to edit", 38, 99);

    drawCardBase(20, 136, 330, 62, COLOR_CARD_BG);
    p_canvas->setTextDatum(TextDatum::TopLeft);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString("Password", 38, 146);

    char password_text[33] = "";
    if (!show_password) {
        for (size_t i = 0; i < strlen(wifi_pass) && i < 32; i++) password_text[i] = '*';
    }
    p_canvas->setTextDatum(TextDatum::MiddleLeft);
    p_canvas->setTextFont(4);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString(show_password ? (wifi_pass[0] ? wifi_pass : "Tap to edit") :
                         (password_text[0] ? password_text : "Tap to edit"), 38, 177);

    drawCardBase(366, 136, 94, 62, show_password ? COLOR_STAT_ON : COLOR_CARD_BG);
    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString(show_password ? "HIDE" : "SHOW", 413, 167);
    p_canvas->setTextDatum(TextDatum::TopLeft);

    drawCardBase(20, 214, 440, 40, COLOR_CARD_BG);
    p_canvas->setTextFont(2);
    p_canvas->setTextDatum(TextDatum::MiddleLeft);
    p_canvas->setTextColor(state.net.wifi_connected ? COLOR_STAT_ON : COLOR_ACCENT_MAIN);
    p_canvas->drawString(state.net.wifi_status_detail[0] ? state.net.wifi_status_detail : "Ready", 38, 234);

    drawCardBase(20, 270, 132, 42, COLOR_CARD_BG);
    drawCardBase(174, 270, 132, 42, COLOR_ACCENT_MAIN);
    drawCardBase(328, 270, 132, 42, COLOR_STAT_ON);

    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->setTextFont(2);
    p_canvas->drawString("RECONNECT", 86, 291);
    p_canvas->drawString("SCAN",      240, 291);
    p_canvas->drawString("CONNECT",   394, 291);
    p_canvas->setTextDatum(TextDatum::TopLeft);
}

void render_wifi_scan(BuildingState& state) {
    drawWallpaperBackground();

    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->setTextFont(4);
    p_canvas->setTextDatum(TextDatum::TopLeft);
    p_canvas->drawString("WiFi Scan", 20, 10);

    drawCardBase(350, 8, 110, 38, COLOR_STAT_OFF);
    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString("BACK", 405, 27);

    p_canvas->setTextFont(2);
    p_canvas->setTextDatum(TextDatum::TopLeft);
    p_canvas->setTextColor(state.net.wifi_scan_error ? COLOR_STAT_ERR :
                           state.net.wifi_scan_radio_warming ? COLOR_STAT_WARN :
                           state.net.wifi_scan_start_pending ? COLOR_STAT_WARN :
                           state.net.wifi_scan_active ? COLOR_STAT_WARN : COLOR_ACCENT_MAIN);
    p_canvas->drawString(state.net.wifi_scan_status, 24, 52);

    uint8_t count = state.net.wifi_scan_count;
    if (count > WIFI_SCAN_MAX_RESULTS) count = WIFI_SCAN_MAX_RESULTS;
    wifi_scan_clamp_scroll(count);

    float diff = wifi_scan_scroll_target - wifi_scan_scroll_y;
    if (diff > -0.5f && diff < 0.5f) wifi_scan_scroll_y = wifi_scan_scroll_target;
    else wifi_scan_scroll_y += diff * 0.35f;

    if (!state.net.wifi_scan_has_results) {
        p_canvas->setTextDatum(TextDatum::MiddleCenter);
        p_canvas->setTextColor(COLOR_TEXT_SEC);
        p_canvas->setTextFont(4);
        p_canvas->drawString((state.net.wifi_scan_active || state.net.wifi_scan_start_pending) ?
                             "Scanning..." : "No Networks", 240, 160);
        p_canvas->setTextFont(2);
        p_canvas->setTextDatum(TextDatum::TopLeft);
    }

    for (uint8_t i = 0; i < count; i++) {
        int ry = WIFI_SCAN_LIST_TOP + (i * WIFI_SCAN_ROW_H) - (int)wifi_scan_scroll_y;
        if (ry + 44 < WIFI_SCAN_LIST_TOP || ry > WIFI_SCAN_LIST_BOTTOM) continue;

        drawCardBase(20, ry, 440, 42, COLOR_CARD_BG);

        p_canvas->setTextDatum(TextDatum::MiddleLeft);
        p_canvas->setTextColor(COLOR_TEXT_MAIN);
        p_canvas->setTextFont(4);
        p_canvas->drawString(state.net.wifi_scan_results[i].ssid, 36, ry + 18);

        char sig_buf[32];
        snprintf(sig_buf, sizeof(sig_buf), "%s %ld",
                 state.net.wifi_scan_results[i].encryption == 0 ? "OPEN" : "LOCK",
                 (long)state.net.wifi_scan_results[i].rssi);
        p_canvas->setTextDatum(TextDatum::MiddleRight);
        p_canvas->setTextFont(2);
        p_canvas->setTextColor(state.net.wifi_scan_results[i].rssi > -67 ? COLOR_STAT_ON : COLOR_STAT_WARN);
        p_canvas->drawString(sig_buf, 444, ry + 24);
    }
    p_canvas->setTextDatum(TextDatum::TopLeft);

    int max_scroll = wifi_scan_max_scroll(count);
    if (max_scroll > 0) {
        int view_h = wifi_scan_view_h();
        int content_h = wifi_scan_content_h(count);
        int thumb_h = (view_h * view_h) / content_h;
        if (thumb_h < 24) thumb_h = 24;
        if (thumb_h > view_h) thumb_h = view_h;

        int travel = view_h - thumb_h;
        int thumb_y = WIFI_SCAN_LIST_TOP + (int)((wifi_scan_scroll_y / max_scroll) * travel);
        p_canvas->fillRoundRect(WIFI_SCAN_SCROLLBAR_X, WIFI_SCAN_LIST_TOP,
                                WIFI_SCAN_SCROLLBAR_W, view_h, 2, COLOR_CARD_BG);
        p_canvas->fillRoundRect(WIFI_SCAN_SCROLLBAR_X, thumb_y,
                                WIFI_SCAN_SCROLLBAR_W, thumb_h, 2, COLOR_ACCENT_MAIN);
    }

    drawCardBase(180, 278, 120, 36, COLOR_CARD_BG);
    drawCardBase(340, 278, 120, 36, COLOR_ACCENT_MAIN);

    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->setTextFont(2);
    p_canvas->drawString("CLEAR",   240, 296);
    p_canvas->drawString("REFRESH", 400, 296);
    p_canvas->setTextDatum(TextDatum::TopLeft);
}

static const char* slave_role_name(uint8_t role) {
    switch (role) {
        case 0x01: return "Temp Node";
        case 0x02: return "Air Node";
        case 0x03: return "Light Node";
        case 0x04: return "Relay Node";
        case 0x05: return "Room Node";
        case 0x06: return "Multi Sensor";
        default: return "Unknown";
    }
}

static DeviceProfile slave_effective_profile(const RS485SlaveState& slave) {
    if (slave.profile != DEVICE_PROFILE_UNASSIGNED) return slave.profile;
    uint16_t mask = slave.enabled_mask ? slave.enabled_mask : slave.capability;
    return device_profile_from_capabilities(mask);
}

static bool slave_profile_allows(const RS485SlaveState& slave, uint16_t capability) {
    DeviceProfile profile = slave_effective_profile(slave);
    uint16_t allowed = device_profile_capability_mask(profile);
    return allowed != 0 && (allowed & capability);
}

static bool pairing_candidate_is_unknown(const RS485State& rs485) {
    if (!rs485.pairing_candidate_ready) return false;
    const RS485SlaveState& candidate = rs485.pairing_candidate;
    if (candidate.mac == 0 && candidate.uid == 0) return true;

    uint8_t count = rs485.slave_count;
    if (count > RS485_MAX_SLAVES) count = RS485_MAX_SLAVES;
    for (uint8_t i = 0; i < count; i++) {
        const RS485SlaveState& slave = rs485.slaves[i];
        if (candidate.mac != 0 && slave.mac == candidate.mac) return false;
        if (candidate.mac == 0 && candidate.uid != 0 && slave.uid == candidate.uid) return false;
    }
    return true;
}

static void slave_capability_label(uint16_t capability, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    auto append_cap = [&](const char* text) {
        size_t used = strlen(out);
        if (used + 1 < out_size) strncat(out, text, out_size - used - 1);
    };
    if (capability & RS485_CAP_TEMP) append_cap("TEMP ");
    if (capability & RS485_CAP_CO2) append_cap("CO2 ");
    if (capability & RS485_CAP_PRESENCE) append_cap("PRES ");
    if (capability & RS485_CAP_LUX) append_cap("LUX ");
    if (capability & RS485_CAP_LIGHT_RELAY) append_cap("REL ");
    if (capability & RS485_CAP_AC_IR) append_cap("AC ");
    if (capability & RS485_CAP_PROJECTOR_IR) append_cap("PROJ ");
    if (capability & RS485_CAP_LCD_CTRL) append_cap("LCD ");
    if (out[0] == '\0') strncpy(out, "-", out_size - 1);
    out[out_size - 1] = '\0';
}

static const char* slave_display_name(const RS485SlaveState& slave) {
    return slave.name[0] ? slave.name : slave_role_name(slave.role);
}

static void slave_age_label(const RS485SlaveState& slave, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    if (slave.last_seen == 0) {
        strncpy(out, "never", out_size - 1);
    } else {
        uint32_t age_s = (millis() - slave.last_seen) / 1000;
        if (age_s < 60) snprintf(out, out_size, "%lus", (unsigned long)age_s);
        else snprintf(out, out_size, "%lum", (unsigned long)(age_s / 60));
    }
    out[out_size - 1] = '\0';
}

static void slave_mac_label(uint64_t mac, char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    if (mac == 0) {
        strncpy(out, "MAC --", out_size - 1);
    } else {
        snprintf(out, out_size, "MAC %02llX:%02llX:%02llX:%02llX:%02llX:%02llX",
                 (unsigned long long)((mac >> 40) & 0xFF),
                 (unsigned long long)((mac >> 32) & 0xFF),
                 (unsigned long long)((mac >> 24) & 0xFF),
                 (unsigned long long)((mac >> 16) & 0xFF),
                 (unsigned long long)((mac >> 8) & 0xFF),
                 (unsigned long long)(mac & 0xFF));
    }
    out[out_size - 1] = '\0';
}

static uint8_t slave_manager_build_indices(const RS485State& rs485, uint8_t* indices, uint8_t max_indices) {
    if (!indices || max_indices == 0) return 0;
    uint8_t count = rs485.slave_count;
    if (count > RS485_MAX_SLAVES) count = RS485_MAX_SLAVES;

    uint8_t visible = 0;
    for (uint8_t i = 0; i < count && visible < max_indices; i++) {
        const RS485SlaveState& slave = rs485.slaves[i];
        if (slave.address != 0 && slave.online) {
            indices[visible++] = i;
        }
    }

    if (visible == 0) {
        indices[visible++] = SLAVE_PLACEHOLDER_INDEX;
    }
    return visible;
}

static bool slave_manager_selection_visible(const uint8_t* indices, uint8_t visible_count) {
    for (uint8_t i = 0; i < visible_count; i++) {
        if (indices[i] != SLAVE_PLACEHOLDER_INDEX && indices[i] == slave_selected_index) return true;
    }
    return false;
}

static bool slave_manager_is_empty(const uint8_t* indices, uint8_t visible_count) {
    return visible_count == 1 && indices[0] == SLAVE_PLACEHOLDER_INDEX;
}

static void draw_slave_manager_focus_panel(const RS485State& rs485, bool empty_list) {
    drawCardBase(38, 146, 404, 132, COLOR_CARD_BG);
    p_canvas->setTextDatum(TextDatum::MiddleCenter);

    if (rs485.pairing_candidate_ready) {
        char cap_buf[64];
        char uid_buf[40];
        slave_capability_label(rs485.pairing_candidate.capability, cap_buf, sizeof(cap_buf));
        snprintf(uid_buf, sizeof(uid_buf), "UID %08lX", (unsigned long)rs485.pairing_candidate.uid);

        p_canvas->setTextFont(4);
        p_canvas->setTextColor(pairing_candidate_is_unknown(rs485) ? COLOR_STAT_WARN : COLOR_STAT_ON);
        p_canvas->drawString(pairing_candidate_is_unknown(rs485) ? "UNPAIRED DEVICE" : "Slave Found", 240, 180);
        p_canvas->setTextFont(2);
        p_canvas->setTextColor(COLOR_TEXT_MAIN);
        p_canvas->drawString(uid_buf, 240, 212);
        p_canvas->setTextColor(COLOR_TEXT_SEC);
        p_canvas->drawString(cap_buf[0] ? cap_buf : "Capability unknown", 240, 236);
        p_canvas->drawString(pairing_candidate_is_unknown(rs485) ?
                             "UNPAIRED_DEVICE_DETECTED" :
                             "Known pairing candidate", 240, 258);
    } else if (rs485.pairing_active) {
        uint32_t timeout_ms = rs485.pairing_timeout_ms;
        uint32_t elapsed_ms = millis() - rs485.pairing_started_ms;
        uint32_t remaining_s = elapsed_ms >= timeout_ms ? 0 : (timeout_ms - elapsed_ms + 999) / 1000;
        char wait_buf[48];
        snprintf(wait_buf, sizeof(wait_buf), "Waiting for pairing... %lus", (unsigned long)remaining_s);

        p_canvas->setTextFont(4);
        p_canvas->setTextColor(COLOR_STAT_WARN);
        p_canvas->drawString("Pairing Mode", 240, 184);
        p_canvas->setTextFont(2);
        p_canvas->setTextColor(COLOR_TEXT_MAIN);
        p_canvas->drawString("Power one slave on address 247", 240, 218);
        p_canvas->setTextColor(COLOR_TEXT_SEC);
        p_canvas->drawString(wait_buf, 240, 246);
    } else if (empty_list) {
        p_canvas->setTextFont(4);
        p_canvas->setTextColor(COLOR_TEXT_MAIN);
        p_canvas->drawString("No Slave Detected", 240, 188);
        p_canvas->setTextFont(2);
        p_canvas->setTextColor(COLOR_TEXT_SEC);
        p_canvas->drawString("Tap DISCOVER then pair a slave", 240, 226);
        p_canvas->drawString("RS485 devices will appear here", 240, 252);
    }

    p_canvas->setTextDatum(TextDatum::TopLeft);
}

static const char* dashboard_logical_name(uint8_t logical_id) {
    switch ((DashboardLogicalId)logical_id) {
        case LOGICAL_TEMP_SLOT_1: return "Temp 1";
        case LOGICAL_TEMP_SLOT_2: return "Temp 2";
        case LOGICAL_TEMP_SLOT_3: return "Temp 3";
        case LOGICAL_TEMP_SLOT_4: return "Temp 4";
        case LOGICAL_CO2_MAIN: return "CO2";
        case LOGICAL_LUX_MAIN: return "Lux";
        case LOGICAL_HUMAN_PRESENCE_MAIN: return "Presence";
        case LOGICAL_AC_CONTROL: return "AC Control";
        case LOGICAL_PROJECTOR_CONTROL: return "Projector";
        default: return "Slot";
    }
}

static const RS485SlaveState* mapping_find_slave(const RS485State& rs485, const LogicalMapping& mapping) {
    uint8_t count = rs485.slave_count;
    if (count > RS485_MAX_SLAVES) count = RS485_MAX_SLAVES;
    for (uint8_t i = 0; i < count; i++) {
        const RS485SlaveState& slave = rs485.slaves[i];
        if (mapping.slave_uid != 0 && slave.uid == mapping.slave_uid) return &slave;
        if (mapping.slave_uid == 0 && mapping.slave_addr != 0 && slave.address == mapping.slave_addr) return &slave;
    }
    return nullptr;
}

static void mapping_source_label(const RS485State& rs485, const LogicalMapping& mapping,
                                 char* out, size_t out_size) {
    if (!out || out_size == 0) return;
    if (!mapping.assigned) {
        snprintf(out, out_size, "--");
        return;
    }

    const RS485SlaveState* slave = mapping_find_slave(rs485, mapping);
    if (slave) {
        snprintf(out, out_size, "%s / Ch %u", slave_display_name(*slave), mapping.channel + 1);
        return;
    }

    snprintf(out, out_size, "Addr 0x%02X / Ch %u", mapping.slave_addr, mapping.channel + 1);
}

static uint8_t mapping_total_pages() {
    uint8_t pages = (DASHBOARD_LOGICAL_SLOT_COUNT + SLAVE_FEATURE_PAGE_ROWS - 1) / SLAVE_FEATURE_PAGE_ROWS;
    return pages == 0 ? 1 : pages;
}

struct FeatureRow {
    const char* label;
    uint16_t capability;
    uint8_t channel;
    DeviceProfile profile;
    bool profile_row;
};

static const FeatureRow SLAVE_FEATURE_ROWS[] = {
    {"TEMP_NODE", 0, 0, TEMP_NODE, true},
    {"PRESENCE_NODE", 0, 0, PRESENCE_NODE, true},
    {"CO2_NODE", 0, 0, CO2_NODE, true},
    {"RELAY_NODE", 0, 0, RELAY_NODE, true},
    {"IR_COMBO_NODE", 0, 0, IR_COMBO_NODE, true},
    {"Temperature 1", RS485_CAP_TEMP, 0, DEVICE_PROFILE_UNASSIGNED, false},
    {"Temperature 2", RS485_CAP_TEMP, 1, DEVICE_PROFILE_UNASSIGNED, false},
    {"Temperature 3", RS485_CAP_TEMP, 2, DEVICE_PROFILE_UNASSIGNED, false},
    {"Temperature 4", RS485_CAP_TEMP, 3, DEVICE_PROFILE_UNASSIGNED, false},
    {"Lux Optional", RS485_CAP_LUX, 0, DEVICE_PROFILE_UNASSIGNED, false},
    {"CO2", RS485_CAP_CO2, 0, DEVICE_PROFILE_UNASSIGNED, false},
    {"Human Presence", RS485_CAP_PRESENCE, 0, DEVICE_PROFILE_UNASSIGNED, false},
    {"Light Relay 1-2", RS485_CAP_LIGHT_RELAY, 0, DEVICE_PROFILE_UNASSIGNED, false},
    {"AC 1+2 Mirror", RS485_CAP_AC_IR, 0, DEVICE_PROFILE_UNASSIGNED, false},
    {"Projector IR", RS485_CAP_PROJECTOR_IR, 0, DEVICE_PROFILE_UNASSIGNED, false}
};

static uint8_t slave_feature_total_count() {
    return sizeof(SLAVE_FEATURE_ROWS) / sizeof(SLAVE_FEATURE_ROWS[0]);
}

static uint8_t slave_feature_total_pages() {
    uint8_t total = slave_feature_total_count();
    uint8_t pages = (total + SLAVE_FEATURE_PAGE_ROWS - 1) / SLAVE_FEATURE_PAGE_ROWS;
    return pages == 0 ? 1 : pages;
}

static uint8_t slave_feature_count_for_capability(const RS485SlaveState& slave, uint16_t capability) {
    if (slave.address == 0 && slave.uid == 0) return 4;
    if (capability == RS485_CAP_TEMP) return slave.temp_count;
    if (capability == RS485_CAP_CO2) return slave.co2_count;
    if (capability == RS485_CAP_PRESENCE) return slave.presence_count;
    if (capability == RS485_CAP_LUX) return slave.lux_count;
    if (capability == RS485_CAP_LIGHT_RELAY) return slave.relay_count;
    if (capability == RS485_CAP_LCD_CTRL) return slave.lcd_count;
    if (capability == RS485_CAP_AC_IR || capability == RS485_CAP_PROJECTOR_IR) return slave.ir_count;
    return 0;
}

static bool temp_channel_enabled_by_other_slave(const RS485State& rs485,
                                                uint8_t current_index,
                                                uint8_t channel) {
    if (channel >= DASHBOARD_TEMP_SLOTS) return false;
    uint8_t count = rs485.slave_count;
    if (count > RS485_MAX_SLAVES) count = RS485_MAX_SLAVES;
    for (uint8_t i = 0; i < count; i++) {
        if (i == current_index) continue;
        const RS485SlaveState& slave = rs485.slaves[i];
        if (slave.online &&
            (slave.enabled_mask & RS485_CAP_TEMP) &&
            (slave.temp_enabled_mask & (1 << channel))) {
            return true;
        }
    }
    return false;
}

static bool capability_enabled_by_other_online_slave(const RS485State& rs485,
                                                     uint8_t current_index,
                                                     uint16_t capability) {
    if (capability == 0 || capability == RS485_CAP_LUX) return false;
    uint8_t count = rs485.slave_count;
    if (count > RS485_MAX_SLAVES) count = RS485_MAX_SLAVES;
    for (uint8_t i = 0; i < count; i++) {
        if (i == current_index) continue;
        const RS485SlaveState& slave = rs485.slaves[i];
        if (slave.online && (slave.enabled_mask & capability)) return true;
    }
    return false;
}

static uint8_t temp_assignable_mask_for_slave(const RS485State& rs485, uint8_t current_index) {
    uint8_t mask = 0;
    for (uint8_t channel = 0; channel < DASHBOARD_TEMP_SLOTS; channel++) {
        if (!temp_channel_enabled_by_other_slave(rs485, current_index, channel)) {
            mask |= (1 << channel);
        }
    }
    return mask;
}

static bool profile_has_assignable_slots(const RS485State& rs485,
                                         uint8_t current_index,
                                         DeviceProfile profile) {
    uint16_t allowed = device_profile_capability_mask(profile);
    uint16_t main_mask = allowed & ~RS485_CAP_LUX;
    if (main_mask == 0) return true;

    if (main_mask & RS485_CAP_TEMP) {
        if (temp_assignable_mask_for_slave(rs485, current_index) == 0) return false;
    }

    const uint16_t single_caps[] = {
        RS485_CAP_CO2,
        RS485_CAP_PRESENCE,
        RS485_CAP_LIGHT_RELAY,
        RS485_CAP_AC_IR,
        RS485_CAP_PROJECTOR_IR
    };
    for (uint8_t i = 0; i < sizeof(single_caps) / sizeof(single_caps[0]); i++) {
        uint16_t capability = single_caps[i];
        if ((main_mask & capability) &&
            capability_enabled_by_other_online_slave(rs485, current_index, capability)) {
            return false;
        }
    }

    return true;
}

static bool slave_feature_is_main(uint16_t capability) {
    return capability != RS485_CAP_LUX;
}

static bool slave_has_other_main_enabled(const RS485SlaveState& slave, uint16_t capability) {
    if (!slave_feature_is_main(capability)) return false;
    uint16_t main_mask = slave.enabled_mask & ~RS485_CAP_LUX;
    if (capability == RS485_CAP_TEMP && (main_mask & RS485_CAP_TEMP)) {
        return false;
    }
    return (main_mask & ~capability) != 0;
}

static bool slave_feature_available(const RS485SlaveState& slave, const FeatureRow& row) {
    if (row.profile_row) return true;
    if (slave.address == 0 && slave.uid == 0) return true;
    if (slave.profile == DEVICE_PROFILE_UNASSIGNED) return true;
    if (!slave_profile_allows(slave, row.capability)) return false;
    if (row.capability == RS485_CAP_TEMP) {
        return row.channel < DASHBOARD_TEMP_SLOTS;
    }
    return true;
}

static bool slave_feature_available_for_state(const RS485State& rs485,
                                              uint8_t current_index,
                                               const RS485SlaveState& slave,
                                               const FeatureRow& row) {
    if (row.profile_row) {
        if (slave_effective_profile(slave) == row.profile) return true;
        return profile_has_assignable_slots(rs485, current_index, row.profile);
    }

    if (!slave_feature_available(slave, row)) return false;
    if (row.capability == RS485_CAP_TEMP) {
        return row.channel < DASHBOARD_TEMP_SLOTS &&
               !temp_channel_enabled_by_other_slave(rs485, current_index, row.channel);
    }
    if (slave_feature_is_main(row.capability)) {
        return !capability_enabled_by_other_online_slave(rs485, current_index, row.capability);
    }
    return true;
}

static bool slave_feature_visible_for_state(const RS485SlaveState& slave, const FeatureRow& row) {
    if (row.profile_row) return true;
    if (slave.profile == DEVICE_PROFILE_UNASSIGNED) return true;

    uint16_t allowed = device_profile_capability_mask(slave.profile);
    return (allowed & row.capability) != 0;
}

static uint8_t slave_feature_visible_count_for_state(const RS485SlaveState& slave) {
    uint8_t visible = 0;
    for (uint8_t i = 0; i < slave_feature_total_count(); i++) {
        if (slave_feature_visible_for_state(slave, SLAVE_FEATURE_ROWS[i])) visible++;
    }
    return visible == 0 ? 1 : visible;
}

static uint8_t slave_feature_total_pages_for_state(const RS485SlaveState& slave) {
    uint8_t total = slave_feature_visible_count_for_state(slave);
    uint8_t pages = (total + SLAVE_FEATURE_PAGE_ROWS - 1) / SLAVE_FEATURE_PAGE_ROWS;
    return pages == 0 ? 1 : pages;
}

static const FeatureRow* slave_feature_visible_at(const RS485SlaveState& slave, uint8_t visible_index) {
    uint8_t cursor = 0;
    for (uint8_t i = 0; i < slave_feature_total_count(); i++) {
        const FeatureRow& row = SLAVE_FEATURE_ROWS[i];
        if (!slave_feature_visible_for_state(slave, row)) continue;
        if (cursor == visible_index) return &row;
        cursor++;
    }
    return nullptr;
}

static bool slave_feature_enabled(const RS485SlaveState& slave, const FeatureRow& row) {
    // Only an explicitly saved profile counts as selected. An inferred profile
    // must remain tappable, otherwise the first tap clears all assignments.
    if (row.profile_row) return slave.profile == row.profile;
    if (!slave_feature_available(slave, row)) return false;
    if (row.capability == RS485_CAP_TEMP) {
        return (slave.enabled_mask & RS485_CAP_TEMP) &&
               (slave.temp_enabled_mask & (1 << row.channel));
    }
    return slave.enabled_mask & row.capability;
}

static uint8_t temp_mask_count(uint8_t mask) {
    uint8_t count = 0;
    mask &= 0x0F;
    for (uint8_t i = 0; i < DASHBOARD_TEMP_SLOTS; i++) {
        if (mask & (1 << i)) count++;
    }
    return count;
}

static void slave_apply_profile_policy(RS485SlaveState& slave, DeviceProfile profile, bool set_defaults) {
    slave.profile = profile;
    uint16_t allowed = device_profile_capability_mask(profile);
    slave.capability = allowed;
    slave.enabled_mask &= allowed;

    if (!(allowed & RS485_CAP_TEMP)) {
        slave.temp_available_mask = 0;
        slave.temp_enabled_mask = 0;
        slave.temp_count = 0;
    } else {
        slave.temp_available_mask = 0x0F;
        slave.temp_count = DASHBOARD_TEMP_SLOTS;
        if (set_defaults) {
            slave.temp_enabled_mask = 0x0F;
            slave.enabled_mask |= RS485_CAP_TEMP;
        }
    }

    if (!(allowed & RS485_CAP_CO2)) slave.co2_count = 0;
    else if (set_defaults || slave.co2_count == 0) slave.co2_count = 1;

    if (!(allowed & RS485_CAP_PRESENCE)) slave.presence_count = 0;
    else if (set_defaults || slave.presence_count == 0) slave.presence_count = 1;

    if (!(allowed & RS485_CAP_LIGHT_RELAY)) slave.relay_count = 0;
    else if (set_defaults || slave.relay_count == 0) slave.relay_count = 2;

    if (!(allowed & (RS485_CAP_AC_IR | RS485_CAP_PROJECTOR_IR))) slave.ir_count = 0;
    else if (set_defaults || slave.ir_count == 0) slave.ir_count = 3;

    if (!(allowed & RS485_CAP_LUX)) {
        slave.lux_count = 0;
        slave.enabled_mask &= ~RS485_CAP_LUX;
    } else {
        if (slave.lux_count == 0) {
            slave.lux_count = 1;
        }
        if (set_defaults) {
            slave.enabled_mask |= RS485_CAP_LUX;
        }
    }

    if (set_defaults) {
        switch (profile) {
            case PRESENCE_NODE: slave.enabled_mask |= RS485_CAP_PRESENCE; break;
            case CO2_NODE: slave.enabled_mask |= RS485_CAP_CO2; break;
            case RELAY_NODE: slave.enabled_mask |= RS485_CAP_LIGHT_RELAY; break;
            case IR_COMBO_NODE: slave.enabled_mask |= RS485_CAP_AC_IR | RS485_CAP_PROJECTOR_IR; break;
            default: break;
        }
    }
}

static void slave_apply_profile_policy_for_state(RS485State& rs485,
                                                 uint8_t current_index,
                                                 RS485SlaveState& slave,
                                                 DeviceProfile profile,
                                                 bool set_defaults) {
    slave_apply_profile_policy(slave, profile, set_defaults);

    uint16_t allowed = device_profile_capability_mask(profile);
    if (allowed & RS485_CAP_TEMP) {
        uint8_t assignable_mask = temp_assignable_mask_for_slave(rs485, current_index);
        slave.temp_available_mask &= assignable_mask;
        slave.temp_enabled_mask &= assignable_mask;
        if (set_defaults) {
            slave.temp_enabled_mask = assignable_mask;
        }
        slave.temp_count = temp_mask_count(slave.temp_available_mask);
        if (slave.temp_enabled_mask == 0) {
            slave.enabled_mask &= ~RS485_CAP_TEMP;
        } else {
            slave.enabled_mask |= RS485_CAP_TEMP;
        }
    }
}

static void slave_clear_profile_selection(RS485SlaveState& slave) {
    slave.profile = DEVICE_PROFILE_UNASSIGNED;
    slave.capability = 0;
    slave.enabled_mask = 0;
    slave.temp_available_mask = 0;
    slave.temp_enabled_mask = 0;
    slave.temp_count = 0;
    slave.co2_count = 0;
    slave.presence_count = 0;
    slave.relay_count = 0;
    slave.ir_count = 0;
    slave.lux_count = 0;
    slave.lcd_count = 0;
}

static void slave_reset_empty_slot(RS485SlaveState& slave) {
    memset(&slave, 0, sizeof(slave));
    strncpy(slave.name, "Device 0", sizeof(slave.name) - 1);
    strncpy(slave.room, "Unassigned", sizeof(slave.room) - 1);
    slave.profile = DEVICE_PROFILE_UNASSIGNED;
    slave.registry_status = DEVICE_STATUS_UNKNOWN;
}

static void slave_forget_locked(BuildingState& state, uint8_t target_index) {
    uint8_t count = state.rs485.slave_count;
    if (count > RS485_MAX_SLAVES) count = RS485_MAX_SLAVES;
    if (target_index >= count) return;

    RS485SlaveState& target = state.rs485.slaves[target_index];
    uint32_t uid = target.uid;
    uint8_t address = target.address;
    if (uid == 0 && address == 0) return;

    for (uint8_t i = 0; i < DASHBOARD_LOGICAL_SLOT_COUNT; i++) {
        LogicalMapping& mapping = state.rs485.mappings[i];
        bool uid_match = uid != 0 && mapping.slave_uid == uid;
        bool addr_match = uid == 0 && address != 0 && mapping.slave_addr == address;
        if (!uid_match && !addr_match) continue;

        mapping.slave_uid = 0;
        mapping.slave_addr = 0;
        mapping.channel = 0;
        mapping.assigned = false;
        mapping.manual_override = false;
    }

    if (count > 1) {
        for (uint8_t i = target_index; i + 1 < count; i++) {
            state.rs485.slaves[i] = state.rs485.slaves[i + 1];
        }
        slave_reset_empty_slot(state.rs485.slaves[count - 1]);
        state.rs485.slave_count = count - 1;
    } else {
        slave_reset_empty_slot(state.rs485.slaves[0]);
        state.rs485.slave_count = 1;
    }

    for (uint8_t i = 0; i < 2; i++) {
        state.sensor.slave_online[i] = i < state.rs485.slave_count && state.rs485.slaves[i].online;
    }
    mapping_manager_update_locked(state);
    state.ui_needs_update = true;
}

static void slave_mark_feature_assignable(RS485SlaveState& slave, const FeatureRow& feature) {
    slave.capability |= feature.capability;
    if (feature.capability == RS485_CAP_TEMP) {
        slave.temp_available_mask |= (1 << feature.channel);
        slave.temp_count = temp_mask_count(slave.temp_available_mask);
    } else if (feature.capability == RS485_CAP_CO2) {
        if (slave.co2_count == 0) slave.co2_count = 1;
    } else if (feature.capability == RS485_CAP_PRESENCE) {
        if (slave.presence_count == 0) slave.presence_count = 1;
    } else if (feature.capability == RS485_CAP_LUX) {
        slave.capability |= RS485_CAP_LUX;
        if (slave.lux_count == 0) slave.lux_count = 1;
    } else if (feature.capability == RS485_CAP_LIGHT_RELAY) {
        if (slave.relay_count == 0) slave.relay_count = 2;
    } else if (feature.capability == RS485_CAP_AC_IR ||
               feature.capability == RS485_CAP_PROJECTOR_IR) {
        if (slave.ir_count == 0) slave.ir_count = 3;
    } else if (feature.capability == RS485_CAP_LCD_CTRL) {
        if (slave.lcd_count == 0) slave.lcd_count = 1;
    }
}

static bool mapping_slave_usable_for_capability(const RS485SlaveState& slave, uint16_t capability) {
    return slave.online &&
           slave.capability_synced &&
           (slave.capability & capability) &&
           (slave.enabled_mask & capability);
}

static uint8_t mapping_channel_count_for_capability(const RS485SlaveState& slave, uint16_t capability) {
    uint8_t count = slave_feature_count_for_capability(slave, capability);
    if (capability != RS485_CAP_TEMP && count > 1) count = 1;
    if (count == 0 && mapping_slave_usable_for_capability(slave, capability)) count = 1;
    if (capability == RS485_CAP_TEMP && count > DASHBOARD_TEMP_SLOTS) count = DASHBOARD_TEMP_SLOTS;
    return count;
}

static bool mapping_channel_enabled_for_capability(const RS485SlaveState& slave,
                                                   uint16_t capability,
                                                   uint8_t channel) {
    if (!mapping_slave_usable_for_capability(slave, capability)) return false;
    if (capability == RS485_CAP_TEMP) {
        return channel < DASHBOARD_TEMP_SLOTS &&
               (slave.temp_available_mask & (1 << channel)) &&
               (slave.temp_enabled_mask & (1 << channel));
    }
    return channel == 0;
}

static uint8_t mapping_source_option_count(const RS485State& rs485, uint16_t capability) {
    uint8_t options = 1; // Clear mapping
    uint8_t count = rs485.slave_count;
    if (count > RS485_MAX_SLAVES) count = RS485_MAX_SLAVES;
    for (uint8_t i = 0; i < count; i++) {
        const RS485SlaveState& slave = rs485.slaves[i];
        if (!mapping_slave_usable_for_capability(slave, capability)) continue;
        uint8_t channels = mapping_channel_count_for_capability(slave, capability);
        for (uint8_t ch = 0; ch < channels; ch++) {
            if (mapping_channel_enabled_for_capability(slave, capability, ch)) options++;
        }
    }
    return options;
}

static bool mapping_source_option_at(const RS485State& rs485, uint16_t capability,
                                     uint8_t option_index, uint8_t* slave_index,
                                     uint8_t* channel) {
    if (option_index == 0) return false;
    uint8_t cursor = 1;
    uint8_t count = rs485.slave_count;
    if (count > RS485_MAX_SLAVES) count = RS485_MAX_SLAVES;
    for (uint8_t i = 0; i < count; i++) {
        const RS485SlaveState& slave = rs485.slaves[i];
        if (!mapping_slave_usable_for_capability(slave, capability)) continue;
        uint8_t channels = mapping_channel_count_for_capability(slave, capability);
        for (uint8_t ch = 0; ch < channels; ch++) {
            if (!mapping_channel_enabled_for_capability(slave, capability, ch)) continue;
            if (cursor == option_index) {
                if (slave_index) *slave_index = i;
                if (channel) *channel = ch;
                return true;
            }
            cursor++;
        }
    }
    return false;
}

static const char* slave_status_label(const RS485SlaveState& slave) {
    if (slave.address == 0 && slave.uid == 0) return "DUMMY";
    if (slave.degraded) return "DEGRADED";
    return slave.online ? "ONLINE" : "OFFLINE";
}

void render_slave_manager(BuildingState& state) {
    drawWallpaperBackground();

    uint8_t visible_indices[RS485_MAX_SLAVES] = {0};
    uint8_t count = slave_manager_build_indices(state.rs485, visible_indices, RS485_MAX_SLAVES);
    bool empty_list = slave_manager_is_empty(visible_indices, count);
    uint8_t real_count = empty_list ? 0 : count;
    uint8_t total_pages = (count + SLAVE_PAGE_ROWS - 1) / SLAVE_PAGE_ROWS;
    if (total_pages == 0) total_pages = 1;
    if (slave_current_page >= total_pages) slave_current_page = total_pages - 1;
    if (!slave_manager_selection_visible(visible_indices, count)) {
        slave_selected_index = visible_indices[0] == SLAVE_PLACEHOLDER_INDEX ? 0 : visible_indices[0];
    }
    uint8_t online_count = 0;
    uint8_t state_count = state.rs485.slave_count;
    if (state_count > RS485_MAX_SLAVES) state_count = RS485_MAX_SLAVES;
    for (uint8_t i = 0; i < state_count; i++) {
        if (state.rs485.slaves[i].online) online_count++;
    }

    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->setTextFont(4);
    p_canvas->setTextDatum(TextDatum::TopLeft);
    p_canvas->drawString(state.rs485.pairing_active ? "Discover Slave" : "Slave Manager", 20, 10);

    drawCardBase(350, 8, 110, 38, COLOR_STAT_OFF);
    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString("BACK", 405, 27);

    p_canvas->setTextFont(2);
    p_canvas->setTextDatum(TextDatum::MiddleRight);
    p_canvas->setTextColor(state.rs485.bus_ok ? COLOR_STAT_ON : COLOR_STAT_ERR);
    p_canvas->drawString(state.rs485.bus_ok ? "BUS OK" : "BUS ERR", 336, 28);
    p_canvas->setTextDatum(TextDatum::TopLeft);

    drawCardBase(20, 58, 440, 38, COLOR_CARD_BG);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString("Modbus RTU 19200", 36, 70);

    char stat_buf[96];
    snprintf(stat_buf, sizeof(stat_buf), "Device:%u  Online:%u  TX:%lu RX:%lu",
             real_count,
             online_count,
             (unsigned long)state.rs485.packets_tx,
             (unsigned long)state.rs485.packets_rx);
    p_canvas->setTextDatum(TextDatum::MiddleRight);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString(stat_buf, 444, 77);
    p_canvas->setTextDatum(TextDatum::TopLeft);

    drawCardBase(20, 104, 112, 38, state.rs485.pairing_active ? COLOR_STAT_WARN : COLOR_ACCENT_MAIN);
    drawCardBase(144, 104, 104, 38, state.rs485.poll_enabled ? COLOR_STAT_ON : COLOR_CARD_BG);
    if (state.rs485.pairing_candidate_ready) {
        drawCardBase(260, 104, 200, 38, COLOR_STAT_ON);
    }
    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString(state.rs485.pairing_active ? "CANCEL" : "DISCOVER", 76, 123);
    p_canvas->drawString(state.rs485.poll_enabled ? "POLL ON" : "POLL OFF", 196, 123);
    if (state.rs485.pairing_candidate_ready) {
        p_canvas->drawString(pairing_candidate_is_unknown(state.rs485) ? "PAIR DEVICE" : "ASSIGN AUTO", 360, 123);
    }
    p_canvas->setTextDatum(TextDatum::TopLeft);

    if (empty_list || state.rs485.pairing_active || state.rs485.pairing_candidate_ready) {
        draw_slave_manager_focus_panel(state.rs485, empty_list);

        drawCardBase(15, 292, 90, 24, COLOR_STAT_OFF);
        p_canvas->setTextDatum(TextDatum::MiddleCenter);
        p_canvas->setTextFont(2);
        p_canvas->setTextColor(COLOR_TEXT_MAIN);
        p_canvas->drawString("BACK", 60, 304);
        p_canvas->setTextDatum(TextDatum::TopLeft);
        return;
    }

    drawCardBase(20, 148, 440, 30, COLOR_CARD_BG);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->setTextFont(2);
    p_canvas->drawString(state.rs485.pairing_active ? "Discovery" :
                         (state.rs485.test_busy ? "Manual test" : "Selected"), 36, 157);
    p_canvas->setTextColor(state.rs485.pairing_active ? COLOR_STAT_WARN : COLOR_TEXT_SEC);
    const char* primary_status = state.rs485.test_busy || state.rs485.test_done_ms > 0 ?
                                 state.rs485.test_status : state.rs485.status;
    if (state.rs485.pairing_active && !state.rs485.pairing_candidate_ready) {
        p_canvas->drawString("Scanning default address 247", 36, 171);
    } else if (!state.rs485.pairing_active && real_count > 0) {
        p_canvas->setTextDatum(TextDatum::MiddleRight);
        p_canvas->drawString("Tap row again for detail", 444, 163);
        p_canvas->setTextDatum(TextDatum::TopLeft);
    } else {
        p_canvas->drawString(primary_status, 36, 171);
    }

    if (state.rs485.test_done_ms > 0 || state.rs485.test_busy) {
        char test_buf[72];
        snprintf(test_buf, sizeof(test_buf), "Result:%s  Seq:%u  Att:%u",
                 state.rs485.test_busy ? "BUSY" : rs485_ui_result_name(state.rs485.test_result),
                 state.rs485.test_seq,
                 state.rs485.test_attempts);
        p_canvas->setTextDatum(TextDatum::MiddleRight);
        p_canvas->setTextColor(state.rs485.test_busy ? COLOR_STAT_WARN :
                               (state.rs485.test_ok ? COLOR_STAT_ON : COLOR_STAT_ERR));
        p_canvas->drawString(test_buf, 444, 163);
        p_canvas->setTextDatum(TextDatum::TopLeft);
    }

    if (state.rs485.pairing_active) {
        uint32_t timeout_ms = state.rs485.pairing_timeout_ms;
        uint32_t elapsed_ms = millis() - state.rs485.pairing_started_ms;
        uint32_t remaining_s = elapsed_ms >= timeout_ms ? 0 : (timeout_ms - elapsed_ms + 999) / 1000;
        char pair_buf[96];
        if (state.rs485.pairing_candidate_ready) {
            char cap_buf[48];
            slave_capability_label(state.rs485.pairing_candidate.capability, cap_buf, sizeof(cap_buf));
            snprintf(pair_buf, sizeof(pair_buf), "UID:%08lX %s",
                     (unsigned long)state.rs485.pairing_candidate.uid,
                     cap_buf);
        } else {
            snprintf(pair_buf, sizeof(pair_buf), "Scan addr 247  %lus", (unsigned long)remaining_s);
        }
        p_canvas->setTextDatum(TextDatum::MiddleRight);
        p_canvas->setTextColor(state.rs485.pairing_candidate_ready ? COLOR_STAT_ON : COLOR_STAT_WARN);
        p_canvas->drawString(pair_buf, 452, 169);
        p_canvas->setTextDatum(TextDatum::TopLeft);
    }

    uint8_t first_row = slave_current_page * SLAVE_PAGE_ROWS;
    uint8_t last_row = first_row + SLAVE_PAGE_ROWS;
    if (last_row > count) last_row = count;

    for (uint8_t row_index = first_row; row_index < last_row; row_index++) {
        uint8_t slave_index = visible_indices[row_index];
        int row_on_page = row_index - first_row;
        int y = SLAVE_LIST_TOP + (row_on_page * SLAVE_ROW_H);
        bool placeholder = slave_index == SLAVE_PLACEHOLDER_INDEX;
        const RS485SlaveState* slave = placeholder ? nullptr : &state.rs485.slaves[slave_index];

        bool selected = !placeholder && slave_index == slave_selected_index;
        uint16_t row_color = selected ? COLOR_ACCENT_SEC : COLOR_CARD_BG;
        drawCardBase(15, y, 450, 30, row_color);

        uint16_t dot = placeholder ? COLOR_STAT_OFF :
                       (slave->degraded ? COLOR_STAT_WARN : (slave->online ? COLOR_STAT_ON : COLOR_STAT_OFF));
        p_canvas->fillCircle(31, y + 15, 5, dot);

        char row[96];
        char cap_buf[56];
        char age_buf[16];
        char mac_buf[36];
        if (placeholder) {
            strncpy(cap_buf, "No slave detected", sizeof(cap_buf) - 1);
            cap_buf[sizeof(cap_buf) - 1] = '\0';
            strncpy(age_buf, "EMPTY", sizeof(age_buf) - 1);
            age_buf[sizeof(age_buf) - 1] = '\0';
            strncpy(mac_buf, "MAC --", sizeof(mac_buf) - 1);
            mac_buf[sizeof(mac_buf) - 1] = '\0';
            snprintf(row, sizeof(row), "Device 0  0x00");
        } else {
            slave_capability_label(slave->capability, cap_buf, sizeof(cap_buf));
            slave_age_label(*slave, age_buf, sizeof(age_buf));
            slave_mac_label(slave->mac, mac_buf, sizeof(mac_buf));
            snprintf(row, sizeof(row), "%s  0x%02X", slave_display_name(*slave), slave->address);
        }
        p_canvas->setTextDatum(TextDatum::MiddleLeft);
        p_canvas->setTextFont(2);
        p_canvas->setTextColor(COLOR_TEXT_MAIN);
        p_canvas->drawString(row, 45, y + 10);

        if (!placeholder && slave->uid != 0) {
            snprintf(row, sizeof(row), "UID:%08lX  %s",
                     (unsigned long)slave->uid,
                     device_profile_name(slave_effective_profile(*slave)));
        } else {
            snprintf(row, sizeof(row), "%s  %s", mac_buf, cap_buf);
        }
        p_canvas->setTextColor(COLOR_TEXT_SEC);
        p_canvas->drawString(row, 45, y + 23);

        p_canvas->setTextDatum(TextDatum::MiddleRight);
        p_canvas->setTextColor(placeholder ? COLOR_TEXT_SEC :
                               (slave->degraded ? COLOR_STAT_WARN : (slave->online ? COLOR_STAT_ON : COLOR_TEXT_SEC)));
        if (placeholder) snprintf(row, sizeof(row), "EMPTY");
        else if (slave->degraded) snprintf(row, sizeof(row), "DEGRADED");
        else if (slave->online) snprintf(row, sizeof(row), "%s", age_buf);
        else snprintf(row, sizeof(row), "OFFLINE");
        p_canvas->setTextFont(2);
        p_canvas->drawString(row, 450, y + 15);
        p_canvas->setTextDatum(TextDatum::TopLeft);
    }

    drawCardBase(15, 292, 90, 24, COLOR_STAT_OFF);
    if (total_pages > 1) drawCardBase(375, 292, 90, 24, COLOR_CARD_BG);
    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString("BACK", 60, 304);
    char page_buf[20];
    snprintf(page_buf, sizeof(page_buf), "Page %u/%u", slave_current_page + 1, total_pages);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString(page_buf, 240, 304);
    if (total_pages > 1) {
        p_canvas->setTextColor(COLOR_TEXT_MAIN);
        p_canvas->drawString("NEXT >", 420, 304);
    }
    p_canvas->setTextDatum(TextDatum::TopLeft);
}

void render_slave_detail(BuildingState& state) {
    p_canvas->fillScreen(COLOR_BG_MAIN);

    RS485SlaveState slave = {};
    bool valid = false;
    uint8_t detail_index = 0;
    data_lock(state);
    if (slave_detail_dummy) {
        slave = state.rs485.slaves[0];
        detail_index = 0;
        valid = true;
    } else if (slave_selected_index >= 0 && slave_selected_index < RS485_MAX_SLAVES) {
        slave = state.rs485.slaves[slave_selected_index];
        detail_index = (uint8_t)slave_selected_index;
        valid = true;
    }
    data_unlock(state);

    if (!valid) {
        screens_set(SCREEN_SLAVE_MANAGER);
        return;
    }

    uint8_t total_pages = slave_feature_total_pages_for_state(slave);
    if (slave_feature_page >= total_pages) slave_feature_page = total_pages - 1;
    if (slave_feature_page < 0) slave_feature_page = 0;

    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->setTextFont(4);
    p_canvas->setTextDatum(TextDatum::TopLeft);
    p_canvas->drawString("Device Detail", 20, 8);
    p_canvas->drawFastHLine(0, 46, 480, COLOR_ACCENT_MAIN);

    drawCardBase(372, 8, 90, 34, COLOR_STAT_OFF);
    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString("BACK", 417, 25);
    p_canvas->setTextDatum(TextDatum::TopLeft);

    drawCardBase(20, 56, 440, 58, COLOR_CARD_BG);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString("Name", 34, 64);
    p_canvas->setTextFont(4);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString(slave_display_name(slave), 34, 82);
    if (!slave_detail_dummy && (slave.address != 0 || slave.uid != 0)) {
        drawCardBase(254, 66, 84, 38, COLOR_STAT_ERR);
    }
    drawCardBase(350, 66, 96, 38, COLOR_CARD_BG);
    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextFont(2);
    if (!slave_detail_dummy && (slave.address != 0 || slave.uid != 0)) {
        p_canvas->setTextColor(COLOR_TEXT_MAIN);
        p_canvas->drawString("DELETE", 296, 85);
    }
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString("EDIT", 398, 85);
    p_canvas->setTextDatum(TextDatum::TopLeft);

    drawCardBase(20, 122, 440, 42, COLOR_CARD_BG);
    char mac_buf[40];
    char id_buf[96];
    slave_mac_label(slave.mac, mac_buf, sizeof(mac_buf));
    snprintf(id_buf, sizeof(id_buf), "Addr 0x%02X  %s  %s",
             slave.address,
             device_profile_name(slave_effective_profile(slave)),
             slave_status_label(slave));
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString(id_buf, 34, 130);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString(mac_buf, 34, 148);

    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString("Device Profile / allowed rows", 24, 172);

    uint8_t first_feature = slave_feature_page * SLAVE_FEATURE_PAGE_ROWS;
    uint8_t total_features = slave_feature_visible_count_for_state(slave);
    for (uint8_t row = 0; row < SLAVE_FEATURE_PAGE_ROWS; row++) {
        uint8_t visible_index = first_feature + row;
        if (visible_index >= total_features) break;

        const FeatureRow* feature_ptr = slave_feature_visible_at(slave, visible_index);
        if (!feature_ptr) break;
        const FeatureRow& feature = *feature_ptr;
        int x = 20;
        int y = 190 + row * 31;
        bool available = slave_feature_available_for_state(state.rs485, detail_index, slave, feature);
        bool enabled = slave_feature_enabled(slave, feature);

        drawCardBase(x, y, 440, 28, available ? COLOR_CARD_BG : COLOR_STAT_OFF);
        p_canvas->drawRoundRect(x + 14, y + 6, 18, 18, 3, available ? COLOR_TEXT_SEC : COLOR_CARD_BG);
        if (enabled) {
            p_canvas->fillRoundRect(x + 18, y + 10, 10, 10, 2, COLOR_STAT_ON);
        }
        p_canvas->setTextDatum(TextDatum::MiddleLeft);
        p_canvas->setTextFont(2);
        p_canvas->setTextColor(available ? COLOR_TEXT_MAIN : COLOR_TEXT_SEC);
        p_canvas->drawString(feature.label, x + 44, y + 14);

        p_canvas->setTextDatum(TextDatum::MiddleRight);
        p_canvas->setTextColor(!available ? COLOR_TEXT_SEC : (enabled ? COLOR_STAT_ON : COLOR_TEXT_SEC));
        p_canvas->drawString(feature.profile_row ?
                             (enabled ? "Selected" : (!available ? "Locked" : "Profile")) :
                             (!available ? "Unavailable" : (enabled ? "Enabled" : "Off")),
                             444, y + 14);
        p_canvas->setTextDatum(TextDatum::TopLeft);
    }

    drawCardBase(356, 286, 104, 28, COLOR_STAT_ON);
    if (total_pages > 1) drawCardBase(144, 286, 190, 28, COLOR_CARD_BG);
    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->setTextFont(2);
    p_canvas->drawString("SAVE", 408, 300);
    if (total_pages > 1) {
        char page_buf[28];
        snprintf(page_buf, sizeof(page_buf), "Page %u/%u  NEXT", slave_feature_page + 1, total_pages);
        p_canvas->setTextColor(COLOR_TEXT_SEC);
        p_canvas->drawString(page_buf, 239, 300);
    }
    p_canvas->setTextDatum(TextDatum::TopLeft);
}

void render_dashboard_mapping(BuildingState& state) {
    p_canvas->fillScreen(COLOR_BG_MAIN);

    uint8_t total_pages = mapping_total_pages();
    if (mapping_page >= total_pages) mapping_page = total_pages - 1;
    if (mapping_page < 0) mapping_page = 0;

    p_canvas->setTextDatum(TextDatum::TopLeft);
    p_canvas->setTextFont(4);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString("Dashboard Mapping", 20, 8);
    p_canvas->drawFastHLine(0, 46, 480, COLOR_ACCENT_MAIN);

    drawCardBase(372, 8, 90, 34, COLOR_STAT_OFF);
    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString("BACK", 417, 25);

    p_canvas->setTextDatum(TextDatum::TopLeft);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString("Dashboard reads logical slots only", 24, 58);

    uint8_t first = mapping_page * SLAVE_FEATURE_PAGE_ROWS;
    for (uint8_t row = 0; row < SLAVE_FEATURE_PAGE_ROWS; row++) {
        uint8_t slot = first + row;
        if (slot >= DASHBOARD_LOGICAL_SLOT_COUNT) break;

        const LogicalMapping& mapping = state.rs485.mappings[slot];
        int y = 84 + row * 62;
        drawCardBase(20, y, 440, 54, COLOR_CARD_BG);

        char source[64];
        mapping_source_label(state.rs485, mapping, source, sizeof(source));

        p_canvas->setTextDatum(TextDatum::MiddleLeft);
        p_canvas->setTextFont(4);
        p_canvas->setTextColor(COLOR_TEXT_MAIN);
        p_canvas->drawString(dashboard_logical_name(mapping.logical_id), 34, y + 22);

        p_canvas->setTextFont(2);
        p_canvas->setTextColor(mapping.assigned ? COLOR_TEXT_MAIN : COLOR_TEXT_SEC);
        p_canvas->drawString(source, 178, y + 18);

        p_canvas->setTextDatum(TextDatum::MiddleRight);
        p_canvas->setTextColor(mapping.assigned ? COLOR_STAT_ON : COLOR_STAT_WARN);
        p_canvas->drawString(mapping.assigned ? (mapping.manual_override ? "Manual" : "Auto") : "NULL", 444, y + 36);
        p_canvas->setTextDatum(TextDatum::TopLeft);
    }

    drawCardBase(20, 286, 118, 28, COLOR_CARD_BG);
    drawCardBase(170, 286, 140, 28, COLOR_CARD_BG);
    drawCardBase(342, 286, 118, 28, COLOR_STAT_ON);

    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString("DETAIL", 79, 300);

    char page_buf[24];
    snprintf(page_buf, sizeof(page_buf), "Page %u/%u", mapping_page + 1, total_pages);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString(page_buf, 240, 300);

    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString("AUTO MAP", 401, 300);
    p_canvas->setTextDatum(TextDatum::TopLeft);
}

void render_mapping_source(BuildingState& state) {
    p_canvas->fillScreen(COLOR_BG_MAIN);

    if (mapping_selected_slot < 0) mapping_selected_slot = 0;
    if (mapping_selected_slot >= DASHBOARD_LOGICAL_SLOT_COUNT) mapping_selected_slot = DASHBOARD_LOGICAL_SLOT_COUNT - 1;

    const LogicalMapping& mapping = state.rs485.mappings[mapping_selected_slot];
    uint16_t capability = mapping.capability_type;
    uint8_t option_count = mapping_source_option_count(state.rs485, capability);
    uint8_t total_pages = (option_count + SLAVE_FEATURE_PAGE_ROWS - 1) / SLAVE_FEATURE_PAGE_ROWS;
    if (total_pages == 0) total_pages = 1;
    if (mapping_source_page >= total_pages) mapping_source_page = total_pages - 1;
    if (mapping_source_page < 0) mapping_source_page = 0;

    p_canvas->setTextDatum(TextDatum::TopLeft);
    p_canvas->setTextFont(4);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString("Select Source", 20, 8);
    p_canvas->drawFastHLine(0, 46, 480, COLOR_ACCENT_MAIN);

    drawCardBase(372, 8, 90, 34, COLOR_STAT_OFF);
    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString("BACK", 417, 25);

    p_canvas->setTextDatum(TextDatum::TopLeft);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString(dashboard_logical_name(mapping.logical_id), 24, 58);

    uint8_t first = mapping_source_page * SLAVE_FEATURE_PAGE_ROWS;
    for (uint8_t row = 0; row < SLAVE_FEATURE_PAGE_ROWS; row++) {
        uint8_t option = first + row;
        if (option >= option_count) break;

        int y = 84 + row * 62;
        bool clear_option = option == 0;
        drawCardBase(20, y, 440, 54, clear_option ? COLOR_STAT_OFF : COLOR_CARD_BG);

        p_canvas->setTextDatum(TextDatum::MiddleLeft);
        p_canvas->setTextFont(4);
        p_canvas->setTextColor(COLOR_TEXT_MAIN);

        if (clear_option) {
            p_canvas->drawString("Clear Mapping", 34, y + 22);
            p_canvas->setTextFont(2);
            p_canvas->setTextColor(COLOR_TEXT_SEC);
            p_canvas->drawString("Slot will show NULL", 34, y + 42);
            continue;
        }

        uint8_t slave_index = 0;
        uint8_t channel = 0;
        if (!mapping_source_option_at(state.rs485, capability, option, &slave_index, &channel)) continue;
        const RS485SlaveState& slave = state.rs485.slaves[slave_index];

        p_canvas->drawString(slave_display_name(slave), 34, y + 20);

        char detail[64];
        snprintf(detail, sizeof(detail), "Addr 0x%02X / Ch %u", slave.address, channel + 1);
        p_canvas->setTextFont(2);
        p_canvas->setTextColor(COLOR_TEXT_SEC);
        p_canvas->drawString(detail, 34, y + 42);

        p_canvas->setTextDatum(TextDatum::MiddleRight);
        p_canvas->setTextColor(COLOR_STAT_ON);
        p_canvas->drawString("SELECT", 444, y + 28);
        p_canvas->setTextDatum(TextDatum::TopLeft);
    }

    if (option_count == 1) {
        p_canvas->setTextDatum(TextDatum::MiddleCenter);
        p_canvas->setTextFont(2);
        p_canvas->setTextColor(COLOR_TEXT_SEC);
        p_canvas->drawString("No enabled source for this slot", 240, 254);
        p_canvas->setTextDatum(TextDatum::TopLeft);
    }

    drawCardBase(20, 286, 118, 28, COLOR_CARD_BG);
    if (total_pages > 1) drawCardBase(170, 286, 140, 28, COLOR_CARD_BG);
    drawCardBase(342, 286, 118, 28, COLOR_STAT_ON);

    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString("MAPPING", 79, 300);

    if (total_pages > 1) {
        char page_buf[24];
        snprintf(page_buf, sizeof(page_buf), "Page %u/%u", mapping_source_page + 1, total_pages);
        p_canvas->setTextColor(COLOR_TEXT_SEC);
        p_canvas->drawString(page_buf, 240, 300);
    }

    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString("NEXT", 401, 300);
    p_canvas->setTextDatum(TextDatum::TopLeft);
}

static void drawLanInfoBox(int x, int y, int w, const char* label, const char* value, bool editable) {
    drawCardBase(x, y, w, 48, editable ? COLOR_CARD_BG : p_canvas->color565(10, 15, 26));
    p_canvas->setTextDatum(TextDatum::TopLeft);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString(label, x + 12, y + 6);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString((value && value[0]) ? value : "-", x + 12, y + 30);
    if (editable) {
        p_canvas->setTextDatum(TextDatum::MiddleRight);
        p_canvas->setTextColor(COLOR_ACCENT_MAIN);
        p_canvas->drawString("EDIT", x + w - 12, y + 24);
        p_canvas->setTextDatum(TextDatum::TopLeft);
    }
}

void render_lan_config(BuildingState& state) {
    drawWallpaperBackground();
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->setTextFont(4);
    p_canvas->setTextDatum(TextDatum::TopLeft);
    p_canvas->drawString("LAN Setup", 20, 10);

    drawCardBase(350, 8, 110, 38, COLOR_STAT_OFF);
    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString("BACK", 405, 27);

    drawCardBase(20, 58, 440, 50, lan_dhcp ? p_canvas->color565(10, 48, 34) : COLOR_CARD_BG);
    p_canvas->setTextDatum(TextDatum::MiddleLeft);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString("Address Mode", 38, 83);
    p_canvas->setTextDatum(TextDatum::MiddleRight);
    p_canvas->setTextFont(4);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString(lan_dhcp ? "DHCP" : "STATIC", 442, 83);

    drawCardBase(20, 122, 440, 48, COLOR_CARD_BG);
    p_canvas->setTextDatum(TextDatum::MiddleLeft);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(state.net.lan_connected ? COLOR_STAT_ON : COLOR_STAT_ERR);
    p_canvas->drawString(state.net.lan_link_status[0] ? state.net.lan_link_status : "-", 38, 140);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    char mode_buf[48];
    if (lan_dhcp && state.net.lan_static_fallback) snprintf(mode_buf, sizeof(mode_buf), "Mode: DHCP -> Static fallback");
    else if (lan_dhcp) snprintf(mode_buf, sizeof(mode_buf), "Mode: DHCP%s", state.net.lan_dhcp_ok ? " lease OK" : "");
    else snprintf(mode_buf, sizeof(mode_buf), "Mode: Static");
    p_canvas->drawString(mode_buf, 38, 158);
    p_canvas->setTextDatum(TextDatum::MiddleRight);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString(state.net.lan_checking ? "Checking..." :
                         (state.net.lan_status_detail[0] ? state.net.lan_status_detail : "-"), 442, 146);
    p_canvas->setTextDatum(TextDatum::TopLeft);

    const char* ip_value = lan_dhcp ? state.net.lan_ip : temp_lan_ip;
    const char* gw_value = lan_dhcp ? state.net.lan_current_gateway : temp_lan_gw;
    const char* sn_value = lan_dhcp ? state.net.lan_current_subnet : temp_lan_sn;
    const char* dns_value = lan_dhcp ? state.net.lan_current_dns : temp_lan_dns;

    drawLanInfoBox(20, 176, 210, "IPv4 Address", ip_value, !lan_dhcp);
    drawLanInfoBox(250, 176, 210, "Default Gateway", gw_value, !lan_dhcp);
    drawLanInfoBox(20, 232, 210, "Subnet Mask", sn_value, !lan_dhcp);
    drawLanInfoBox(250, 232, 210, "DNS Server", dns_value, !lan_dhcp);
    
    p_canvas->setTextDatum(TextDatum::TopLeft);

    drawCardBase(180, 288, 120, 26, COLOR_CARD_BG);
    drawCardBase(340, 288, 120, 26, COLOR_STAT_ON);

    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->setTextFont(2);
    p_canvas->drawString(lan_dhcp ? "AUTO" : "EDIT IP", 240, 301);
    p_canvas->drawString("SAVE", 400, 301);
    p_canvas->setTextDatum(TextDatum::TopLeft);
}

void screens_render(BuildingState& state, int fps) {
    switch (current_screen) {
        case SCREEN_DASHBOARD:   render_dashboard(state, fps); break;
        case SCREEN_SETTINGS:    render_settings(state);       break;
        case SCREEN_WIFI_CONFIG: render_wifi_config(state);    break;
        case SCREEN_LAN_CONFIG:  render_lan_config(state);     break;
        case SCREEN_WIFI_SCAN:   render_wifi_scan(state);      break;
        case SCREEN_SLAVE_MANAGER: render_slave_manager(state); break;
        case SCREEN_SLAVE_DETAIL: render_slave_detail(state);  break;
        case SCREEN_DASHBOARD_MAPPING: render_dashboard_mapping(state); break;
        case SCREEN_MAPPING_SOURCE: render_mapping_source(state); break;
        case SCREEN_TEMP_DETAIL: render_temperature_detail(state); break;
        case SCREEN_DEVICE_INFO: render_device_info(state); break;
        case SCREEN_MQTT_SETUP: render_mqtt_setup(state); break;
        case SCREEN_TOUCH_TEST: {
            p_canvas->fillScreen(COLOR_BG_MAIN);
            p_canvas->setTextDatum(TextDatum::TopLeft);
            p_canvas->setTextFont(2);
            p_canvas->setTextColor(COLOR_TEXT_MAIN);
            p_canvas->drawString("TOUCH ALIGNMENT TEST", 12, 8);
            p_canvas->setTextColor(COLOR_TEXT_SEC);
            p_canvas->drawString("Coordinates shown are screen pixels only (0..479, 0..319)", 12, 28);

            for (int x = 40; x < 480; x += 40) p_canvas->fillRect(x, 52, 1, 268, COLOR_CARD_BG);
            for (int y = 80; y < 320; y += 40) p_canvas->drawFastHLine(0, y, 480, COLOR_CARD_BG);

            struct TouchTarget { int x; int y; };
            const TouchTarget targets[] = {
                {35, 70}, {240, 70}, {445, 70},
                {35, 180},           {445, 180},
                {35, 290}, {240, 290}, {445, 290}
            };
            p_canvas->setTextFont(1);
            p_canvas->setTextDatum(TextDatum::MiddleCenter);
            for (const auto& target : targets) {
                p_canvas->drawRoundRect(target.x - 7, target.y - 7, 15, 15, 7, COLOR_STAT_WARN);
                p_canvas->drawFastHLine(target.x - 11, target.y, 23, COLOR_STAT_WARN);
                p_canvas->fillRect(target.x, target.y - 11, 1, 23, COLOR_STAT_WARN);

                char label[20];
                snprintf(label, sizeof(label), "%d,%d", target.x, target.y);
                int label_y = target.y < 100 ? target.y + 18 : target.y - 18;
                p_canvas->setTextColor(COLOR_TEXT_MAIN);
                p_canvas->drawString(label, target.x, label_y);
            }
            p_canvas->setTextDatum(TextDatum::TopLeft);

            data_lock(state);
            bool pressed = state.touch_pressed;
            int tx = state.touch_x;
            int ty = state.touch_y;
            int lx = state.touch_last_x;
            int ly = state.touch_last_y;
            data_unlock(state);

            char coord[40];
            int show_x = pressed ? tx : lx;
            int show_y = pressed ? ty : ly;
            snprintf(coord, sizeof(coord), "X:%d Y:%d %s", show_x, show_y,
                     pressed ? "TOUCH" : "LAST");
            p_canvas->setTextColor(pressed ? COLOR_STAT_ON : COLOR_TEXT_SEC);
            p_canvas->drawString(coord, 270, 8);

            if (show_x >= 0 && show_x < 480 && show_y >= 0 && show_y < 320) {
                p_canvas->drawRoundRect(show_x - 10, show_y - 10, 21, 21, 10, COLOR_STAT_ON);
                p_canvas->drawFastHLine(show_x - 15, show_y, 31, COLOR_STAT_ON);
                p_canvas->fillRect(show_x, show_y - 15, 1, 31, COLOR_STAT_ON);
            }

            p_canvas->setTextDatum(TextDatum::TopLeft);
            break;
        }
        case SCREEN_CLOCK_SETUP: render_clock_setup(state);    break;
        case SCREEN_KEYBOARD:    keyboard_draw();              break;
        default: break;
    }
}

bool screens_has_animation() {
    if (current_screen == SCREEN_WIFI_SCAN) {
        float diff = wifi_scan_scroll_target - wifi_scan_scroll_y;
        return diff > 0.5f || diff < -0.5f;
    }

    if (current_screen == SCREEN_SLAVE_MANAGER) {
        float diff = slave_scroll_target - slave_scroll_y;
        return diff > 0.5f || diff < -0.5f;
    }



    return false;
}

// ── Touch Handlers ────────────────────────────────────────────────────────────

void handle_dashboard_touch_legacy(BuildingState& state, int tx, int ty) {
    Serial.printf("[TOUCH] Dashboard tx:%d ty:%d\n", tx, ty);

    // Hamburger — kiri atas di notif bar
    if (isHit(tx, ty, 0, 0, 50, 35)) {
        screens_set(SCREEN_SETTINGS); return;
    }
    // Page button — kanan bawah
    if (isHit(tx, ty, 370, 287, 105, 33)) {
        data_lock(state);
        state.dashboard_page = (state.dashboard_page == 0) ? 1 : 0;
        state.ui_needs_update = true;
        data_unlock(state);
        return;
    }

    if (state.dashboard_page == 0) {
        bool send_ac = false;
        bool send_projector = false;
        bool send_light = false;
        bool ac_power = false;
        float ac_target = 0.0f;
        uint8_t ac_fan = 0;
        uint8_t ac_swing = 0;
        bool projector_power = false;
        bool light_power = false;
        data_lock(state);
        if      (isHit(tx, ty, 20, 145, 95, 55))  { state.sensor.temp_target = min(30.0f, state.sensor.temp_target + 1.0f); send_ac = true; }
        else if (isHit(tx, ty, 125, 145, 95, 55)) { state.sensor.temp_target = max(16.0f, state.sensor.temp_target - 1.0f); send_ac = true; }
        else if (isHit(tx, ty, 20, 210, 200, 50)) { state.sensor.ac_on        = !state.sensor.ac_on; send_ac = true; }
        else if (isHit(tx, ty, 240, 203, 110, 47)){ state.sensor.projector_on = !state.sensor.projector_on; send_projector = true; }
        else if (isHit(tx, ty, 362, 203, 110, 47)){ state.sensor.light_on     = !state.sensor.light_on; send_light = true; }
        if (send_ac) state.sensor.app_controlled_ac = false;
        if (send_projector) state.sensor.app_controlled_projector = false;
        if (send_light) state.sensor.app_controlled_light = false;
        ac_power = state.sensor.ac_on;
        ac_target = state.sensor.temp_target;
        ac_fan = state.sensor.ac_fan_speed;
        ac_swing = state.sensor.ac_swing_mode;
        projector_power = state.sensor.projector_on;
        light_power = state.sensor.light_on;
        state.ui_needs_update = true;
        data_unlock(state);
        if (send_ac) rs485_request_ac_command(ac_power, ac_target, 0, ac_fan, ac_swing);
        if (send_projector) rs485_request_projector_command(projector_power);
        if (send_light) rs485_request_light_command(light_power);
    }
}

void handle_dashboard_touch(BuildingState& state, int tx, int ty) {
    Serial.printf("[TOUCH] Dashboard tx:%d ty:%d\n", tx, ty);

    DashboardUiModel model = dashboard_make_ui_model(state);

    if (isHit(tx, ty, 0, 0, 140, 48)) {
        screens_set(SCREEN_SETTINGS);
        return;
    }

    UiRect temp_rect = {0, 0, 0, 0};
    if (model.layout == DASH_LAYOUT_TEMP_CENTER_LARGE) {
        temp_rect = {40, 78, 400, 174};
    } else if (model.layout == DASH_LAYOUT_TEMP_COMPACT_WITH_CONTROLS) {
        if (dashboard_is_full_control_layout(model)) {
            temp_rect = {190, 48, 100, 34};
        } else if (model.has_ac && !model.has_projector && !model.has_led) {
            temp_rect = {24, 104, 204, 124};
        } else if (model.has_ac && model.has_projector && !model.has_led) {
            temp_rect = {252, 214, 216, 84};
        } else if (model.has_ac) {
            temp_rect = {24, 72, 204, 104};
        } else {
            temp_rect = {88, 72, 304, 104};
        }
    }
    if (model.has_temp && temp_rect.w > 0 && hit_rect(tx, ty, temp_rect)) {
        screens_set(SCREEN_TEMP_DETAIL);
        return;
    }

    UiRect ac_rect = {0, 0, 0, 0};
    UiRect projector_rect = {0, 0, 0, 0};
    UiRect led_rect = {0, 0, 0, 0};
    UiRect led1_rect = {0, 0, 0, 0};
    UiRect led2_rect = {0, 0, 0, 0};

    switch (model.layout) {
        case DASH_LAYOUT_TEMP_COMPACT_WITH_CONTROLS:
            if (dashboard_is_full_control_layout(model)) {
                ac_rect = {18, 88, 224, 210};
                projector_rect = {258, 88, 204, 94};
                if (model.led_channel_count > 1) {
                    led1_rect = {258, 194, 98, 104};
                    led2_rect = {364, 194, 98, 104};
                } else {
                    led1_rect = {258, 194, 204, 104};
                }
            } else if (model.has_ac && !model.has_projector && !model.has_led) {
                ac_rect = {252, 104, 204, 124};
            } else if (model.has_ac && model.has_projector && !model.has_led) {
                ac_rect = {18, 64, 216, 234};
                projector_rect = {306, 64, 136, 136};
            } else if (model.has_ac) {
                ac_rect = {252, 72, 204, 104};
            }
            if (dashboard_is_full_control_layout(model)) {
                // Already assigned above
            } else if (model.has_ac && model.has_projector && !model.has_led) {
                // Rects are assigned above for the dedicated elderly-friendly layout.
            } else if (model.has_projector && model.has_led) {
                projector_rect = {24, 190, 204, 104};
                led_rect = {252, 190, 204, 104};
            } else if (model.has_projector) {
                projector_rect = {88, 190, 304, 104};
            } else if (model.has_led) {
                led_rect = {88, 190, 304, 104};
            }
            break;

        case DASH_LAYOUT_SINGLE_CONTROL_CENTER:
            if (model.has_ac) ac_rect = {98, 88, 284, 150};
            else if (model.has_projector) projector_rect = {72, 92, 336, 144};
            else if (model.has_led) led_rect = {72, 92, 336, 144};
            break;

        case DASH_LAYOUT_MULTI_CONTROL_SPLIT:
            if (model.has_ac && model.has_projector && !model.has_led) {
                ac_rect = {18, 62, 224, 236};
                projector_rect = {258, 75, 210, 210};
            } else if (model.has_ac) {
                ac_rect = {24, 76, 206, 142};
                if (model.has_projector) projector_rect = {250, 76, 206, 74};
                if (model.has_led) led_rect = {250, 158, 206, 74};
            } else {
                projector_rect = {24, 92, 204, 144};
                led_rect = {252, 92, 204, 144};
            }
            break;

        default:
            break;
    }

    if (led_rect.w > 0 && model.led_channel_count > 1) {
        int gap = 8;
        int channel_w = (led_rect.w - gap) / 2;
        led1_rect = {led_rect.x, led_rect.y, channel_w, led_rect.h};
        led2_rect = {led_rect.x + channel_w + gap, led_rect.y,
                     led_rect.w - channel_w - gap, led_rect.h};
        led_rect = {0, 0, 0, 0};
    }

    if (ac_rect.w > 0) {
        bool expanded_controls = ac_rect.h >= 180;
        int btn_h = expanded_controls ? 52 : 34;
        int btn_y = expanded_controls ? ac_rect.y + 90 : ac_rect.y + ac_rect.h - btn_h - 10;
        int btn_w = (ac_rect.w - 30) / 2;
        int chip_x = ac_rect.x + ac_rect.w - 88;
        int chip_y = ac_rect.y + 14;
        int chip_h = expanded_controls ? 64 : btn_y - chip_y - 4;
        if (chip_h < 42) chip_h = 42;
        UiRect power_rect = {chip_x, chip_y, 74, chip_h};
        UiRect up_rect = {ac_rect.x + 10, btn_y, btn_w, btn_h};
        UiRect down_rect = {ac_rect.x + 20 + btn_w, btn_y, btn_w, btn_h};
        UiRect swing_rect = {ac_rect.x + 10, ac_rect.y + ac_rect.h - 62, btn_w, 52};
        UiRect fan_rect = {ac_rect.x + 20 + btn_w, ac_rect.y + ac_rect.h - 62, btn_w, 52};

        if (hit_rect(tx, ty, power_rect)) {
            bool ac_power = false;
            float ac_target = 0.0f;
            uint8_t ac_fan = 0;
            uint8_t ac_swing = 0;
            data_lock(state);
            state.sensor.ac_on = !state.sensor.ac_on;
            state.sensor.app_controlled_ac = false;
            ac_power = state.sensor.ac_on;
            ac_target = state.sensor.temp_target;
            ac_fan = state.sensor.ac_fan_speed;
            ac_swing = state.sensor.ac_swing_mode;
            state.ui_needs_update = true;
            data_unlock(state);
            rs485_request_ac_command(ac_power, ac_target, 0, ac_fan, ac_swing);
            return;
        }

        if (hit_rect(tx, ty, up_rect) || hit_rect(tx, ty, down_rect)) {
            bool ac_power = false;
            float ac_target = 0.0f;
            uint8_t ac_fan = 0;
            uint8_t ac_swing = 0;
            data_lock(state);
            if (hit_rect(tx, ty, up_rect)) {
                state.sensor.temp_target = min(30.0f, state.sensor.temp_target + 1.0f);
            } else {
                state.sensor.temp_target = max(16.0f, state.sensor.temp_target - 1.0f);
            }
            state.sensor.app_controlled_ac = false;
            ac_power = state.sensor.ac_on;
            ac_target = state.sensor.temp_target;
            ac_fan = state.sensor.ac_fan_speed;
            ac_swing = state.sensor.ac_swing_mode;
            state.ui_needs_update = true;
            data_unlock(state);
            rs485_request_ac_command(ac_power, ac_target, 0, ac_fan, ac_swing);
            return;
        }

        if (expanded_controls && hit_rect(tx, ty, swing_rect)) {
            bool ac_power = false;
            float ac_target = 0.0f;
            uint8_t ac_fan = 0;
            uint8_t ac_swing = 0;
            data_lock(state);
            state.sensor.ac_swing_mode = (state.sensor.ac_swing_mode + 1) % 7;
            state.sensor.app_controlled_ac = false;
            ac_power = state.sensor.ac_on;
            ac_target = state.sensor.temp_target;
            ac_fan = state.sensor.ac_fan_speed;
            ac_swing = state.sensor.ac_swing_mode;
            state.ui_needs_update = true;
            data_unlock(state);
            Serial.printf("[UI] AC swing mode: %u\n", ac_swing);
            rs485_request_ac_command(ac_power, ac_target, 0, ac_fan, ac_swing);
            return;
        }

        if (expanded_controls && hit_rect(tx, ty, fan_rect)) {
            bool ac_power = false;
            float ac_target = 0.0f;
            uint8_t ac_fan = 0;
            uint8_t ac_swing = 0;
            data_lock(state);
            state.sensor.ac_fan_speed = (state.sensor.ac_fan_speed + 1) % 6;
            state.sensor.app_controlled_ac = false;
            ac_power = state.sensor.ac_on;
            ac_target = state.sensor.temp_target;
            ac_fan = state.sensor.ac_fan_speed;
            ac_swing = state.sensor.ac_swing_mode;
            state.ui_needs_update = true;
            data_unlock(state);
            Serial.printf("[UI] AC fan speed: %u\n", ac_fan);
            rs485_request_ac_command(ac_power, ac_target, 0, ac_fan, ac_swing);
            return;
        }
    }

    if (projector_rect.w > 0 && hit_rect(tx, ty, projector_rect)) {
        bool projector_power = false;
        data_lock(state);
        state.sensor.projector_on = !state.sensor.projector_on;
        state.sensor.app_controlled_projector = false;
        projector_power = state.sensor.projector_on;
        state.ui_needs_update = true;
        data_unlock(state);
        rs485_request_projector_command(projector_power);
        return;
    }

    if (led_rect.w > 0 && hit_rect(tx, ty, led_rect)) {
        bool light_power = false;
        data_lock(state);
        state.sensor.light_on = !state.sensor.light_on;
        state.sensor.app_controlled_light = false;
        light_power = state.sensor.light_on;
        state.ui_needs_update = true;
        data_unlock(state);
        rs485_request_light_command(light_power);
        return;
    }

    if (led1_rect.w > 0 && hit_rect(tx, ty, led1_rect)) {
        data_lock(state);
        state.sensor.app_controlled_light = false;
        data_unlock(state);
        rs485_request_light_channel_command(1, !model.led_channel_on[0]);
        return;
    }

    if (led2_rect.w > 0 && hit_rect(tx, ty, led2_rect)) {
        data_lock(state);
        state.sensor.app_controlled_light = false;
        data_unlock(state);
        rs485_request_light_channel_command(2, !model.led_channel_on[1]);
        return;
    }
}

void handle_temperature_detail_touch(BuildingState& state, int tx, int ty) {
    (void)state;
    if (isHit(tx, ty, 372, 8, 90, 34)) {
        screens_set(SCREEN_DASHBOARD);
    }
}

void handle_dashboard_touch_event(BuildingState& state, int tx, int ty, TouchEventType event) {
    if (event == TOUCH_EVENT_DOWN) {
        dashboard_dragging = true;
        dashboard_moved = false;
        dashboard_drag_start_y = ty;

        if (dashboard_env_panel && hit_rect(tx, ty, dashboard_env_close_rect())) {
            dashboard_env_panel = false;
            data_lock(state);
            state.ui_needs_update = true;
            data_unlock(state);
            dashboard_dragging = false;
        }
        return;
    }

    if (event == TOUCH_EVENT_MOVE && dashboard_dragging) {
        if (abs(ty - dashboard_drag_start_y) > 14) dashboard_moved = true;
        return;
    }

    if (event == TOUCH_EVENT_UP && dashboard_dragging) {
        int dy = ty - dashboard_drag_start_y;
        dashboard_dragging = false;

        if (dy < -44) {
            dashboard_env_panel = true;
            data_lock(state);
            state.ui_needs_update = true;
            data_unlock(state);
            return;
        }

        if (dy > 44 && dashboard_env_panel) {
            dashboard_env_panel = false;
            data_lock(state);
            state.ui_needs_update = true;
            data_unlock(state);
            return;
        }

        if (!dashboard_moved && !dashboard_env_panel) {
            handle_dashboard_touch(state, tx, ty);
        }
    }
}

void handle_settings_touch_event(BuildingState& state, int tx, int ty, TouchEventType event) {
    if (event == TOUCH_EVENT_DOWN) {
        settings_dragging = true;
        settings_moved = false;
        settings_drag_start_x = tx;
        settings_drag_start_y = ty;

        if (isHit(tx, ty, 338, 8, 122, 38)) {
            screens_set(SCREEN_DASHBOARD);
            settings_dragging = false;
            return;
        }
        return;
    }

    if (event == TOUCH_EVENT_MOVE && settings_dragging) {
        if (abs(tx - settings_drag_start_x) > 10 || abs(ty - settings_drag_start_y) > 10) {
            settings_moved = true;
        }
        return;
    }

    if (event == TOUCH_EVENT_UP && settings_dragging) {
        settings_dragging = false;
        int dx = tx - settings_drag_start_x;

        // Horizontal swipe detection
        if (dx > 50) {
            // Swipe right -> Page 1
            if (settings_page == 1) {
                settings_page = 0;
                data_lock(state);
                state.ui_needs_update = true;
                data_unlock(state);
                return;
            }
        } else if (dx < -50) {
            // Swipe left -> Page 2
            if (settings_page == 0) {
                settings_page = 1;
                data_lock(state);
                state.ui_needs_update = true;
                data_unlock(state);
                return;
            }
        }

        // Tap handling if no swipe/move occurred
        if (!settings_moved) {
            if (settings_page == 0) {
                // PAGE 1 Items
                // Network Priority Toggle: 20, 58, 440, 48
                if (isHit(tx, ty, 20, 58, 440, 48)) {
                    data_lock(state);
                    state.net.net_priority = (state.net.net_priority == 0) ? 1 : 0;
                    state.ui_needs_update = true;
                    data_unlock(state);
                    if (ui_callbacks.onPriorityChange) {
                        ui_callbacks.onPriorityChange(state.net.net_priority);
                    }
                }
                // WiFi Setup: 20, 126, 136, 92
                else if (isHit(tx, ty, 20, 126, 136, 92)) {
                    data_lock(state);
                    strncpy(wifi_ssid, state.net.saved_wifi_ssid, sizeof(wifi_ssid) - 1);
                    wifi_ssid[sizeof(wifi_ssid) - 1] = '\0';
                    strncpy(wifi_pass, state.net.saved_wifi_pass, sizeof(wifi_pass) - 1);
                    wifi_pass[sizeof(wifi_pass) - 1] = '\0';
                    data_unlock(state);
                    screens_set(SCREEN_WIFI_CONFIG);
                }
                // LAN Setup: 172, 126, 136, 92
                else if (isHit(tx, ty, 172, 126, 136, 92)) {
                    data_lock(state);
                    lan_dhcp = state.net.lan_use_dhcp;
                    strncpy(temp_lan_ip, state.net.lan_static_ip, 15);
                    strncpy(temp_lan_gw, state.net.lan_gateway, 15);
                    strncpy(temp_lan_sn, state.net.lan_subnet, 15);
                    strncpy(temp_lan_dns, state.net.lan_dns, 15);
                    data_unlock(state);
                    screens_set(SCREEN_LAN_CONFIG);
                }
                // Slave Manager: 324, 126, 136, 92
                else if (isHit(tx, ty, 324, 126, 136, 92)) {
                    screens_set(SCREEN_SLAVE_MANAGER);
                }
            } else {
                // PAGE 2 Items
                // MQTT Setup: 20, 58, 440, 58
                if (isHit(tx, ty, 20, 58, 440, 58)) {
                    screens_set(SCREEN_MQTT_SETUP);
                }
                // Device Info: 20, 126, 440, 58
                else if (isHit(tx, ty, 20, 126, 440, 58)) {
                    screens_set(SCREEN_DEVICE_INFO);
                }
                // Clock Setup: 20, 194, 440, 58
                else if (isHit(tx, ty, 20, 194, 440, 58)) {
                    clock_setup_manual_mode = state.net.use_manual_time;
                    struct tm timeinfo;
                    if (getLocalTime(&timeinfo, 5)) {
                        manual_clock_year   = timeinfo.tm_year + 1900;
                        manual_clock_month  = timeinfo.tm_mon + 1;
                        manual_clock_day    = timeinfo.tm_mday;
                        manual_clock_hour   = timeinfo.tm_hour;
                        manual_clock_minute = timeinfo.tm_min;
                    } else {
                        manual_clock_year   = 2026;
                        manual_clock_month  = 6;
                        manual_clock_day    = 17;
                        manual_clock_hour   = 19;
                        manual_clock_minute = 47;
                    }
                    screens_set(SCREEN_CLOCK_SETUP);
                }
            }
        }
    }
}

void handle_device_info_touch(BuildingState& state, int tx, int ty) {
    if (isHit(tx, ty, 338, 8, 122, 38)) {
        screens_set(SCREEN_SETTINGS);
        return;
    }
    if (isHit(tx, ty, 20, 58, 440, 48)) {
        editing_target = 8;
        keyboard_set_text(state.net.device_name);
        screens_set(SCREEN_KEYBOARD);
        return;
    }
    if (isHit(tx, ty, 20, 118, 210, 54)) {
        editing_target = 9;
        keyboard_set_text(state.net.class_name);
        screens_set(SCREEN_KEYBOARD);
        return;
    }
}

void handle_mqtt_setup_touch(BuildingState& state, int tx, int ty) {
    if (isHit(tx, ty, 350, 8, 110, 38)) {
        screens_set(SCREEN_SETTINGS);
        return;
    }
    if (isHit(tx, ty, 20, 54, 440, 48)) {
        editing_target = 10;
        keyboard_set_text(state.net.mqtt_server);
        screens_set(SCREEN_KEYBOARD);
        return;
    }
    if (isHit(tx, ty, 20, 110, 210, 48)) {
        char port[8];
        snprintf(port, sizeof(port), "%u", state.net.mqtt_port);
        editing_target = 11;
        keyboard_set_text(port);
        screens_set(SCREEN_KEYBOARD);
        return;
    }
    if (isHit(tx, ty, 250, 110, 210, 48)) {
        data_lock(state);
        state.net.mqtt_use_tls = !state.net.mqtt_use_tls;
        state.ui_needs_update = true;
        data_unlock(state);
        data_save_device_config(state);
        return;
    }
    if (isHit(tx, ty, 20, 166, 440, 44)) {
        editing_target = 12;
        keyboard_set_text(state.net.mqtt_user);
        screens_set(SCREEN_KEYBOARD);
        return;
    }
    if (isHit(tx, ty, 20, 218, 440, 44)) {
        editing_target = 13;
        keyboard_set_text(state.net.mqtt_pass);
        screens_set(SCREEN_KEYBOARD);
        return;
    }
    if (isHit(tx, ty, 250, 274, 210, 38)) {
        data_save_device_config(state);
        mqtt_request_reconnect();
        screens_set(SCREEN_SETTINGS);
    }
}

void handle_wifi_touch(BuildingState& state, int tx, int ty) {
    if      (isHit(tx, ty, 350, 8, 110, 38))   { screens_set(SCREEN_SETTINGS); }
    else if (isHit(tx, ty, 20, 58, 440, 62))   { editing_target = 1; keyboard_set_text(wifi_ssid); screens_set(SCREEN_KEYBOARD); }
    else if (isHit(tx, ty, 20, 136, 330, 62))  { editing_target = 2; keyboard_set_text(wifi_pass); screens_set(SCREEN_KEYBOARD); }
    else if (isHit(tx, ty, 366, 136, 94, 62))  { show_password = !show_password; data_lock(state); state.ui_needs_update = true; data_unlock(state); }
    else if (isHit(tx, ty, 20, 270, 132, 42)) { if (ui_callbacks.onWiFiReconnect) ui_callbacks.onWiFiReconnect(); }
    else if (isHit(tx, ty, 174, 270, 132, 42)) {
        wifi_scan_scroll_y = 0;
        wifi_scan_scroll_target = 0;
        if (ui_callbacks.onWiFiScan) ui_callbacks.onWiFiScan();
        screens_set(SCREEN_WIFI_SCAN);
    }
    else if (isHit(tx, ty, 328, 270, 132, 42)) { if (ui_callbacks.onWiFiConnect) ui_callbacks.onWiFiConnect(wifi_ssid, wifi_pass); screens_set(SCREEN_DASHBOARD); }
}

void handle_wifi_scan_touch(BuildingState& state, int tx, int ty) {
    if (isHit(tx, ty, 350, 8, 110, 38)) { screens_set(SCREEN_WIFI_CONFIG); }
    else if (isHit(tx, ty, 180, 278, 120, 36)) {
        wifi_scan_scroll_y = 0;
        wifi_scan_scroll_target = 0;
        data_lock(state);
        state.ui_needs_update = true;
        data_unlock(state);
    }
    else if (isHit(tx, ty, 340, 278, 120, 36)) {
        wifi_scan_scroll_y = 0;
        wifi_scan_scroll_target = 0;
        if (ui_callbacks.onWiFiScan) ui_callbacks.onWiFiScan();
    }
    // Tap a network to auto-fill SSID
    else {
        uint8_t count = state.net.wifi_scan_count;
        if (count > WIFI_SCAN_MAX_RESULTS) count = WIFI_SCAN_MAX_RESULTS;

        for (uint8_t i = 0; i < count; i++) {
            int ry = WIFI_SCAN_LIST_TOP + (i * WIFI_SCAN_ROW_H) - (int)wifi_scan_scroll_y;
            if (isHit(tx, ty, 20, ry, 440, 42)) {
                strncpy(wifi_ssid, state.net.wifi_scan_results[i].ssid, sizeof(wifi_ssid) - 1);
                wifi_ssid[sizeof(wifi_ssid) - 1] = '\0';
                screens_set(SCREEN_WIFI_CONFIG);
                break;
            }
        }
    }
}

void handle_wifi_scan_touch_event(BuildingState& state, int tx, int ty, TouchEventType event) {
    uint8_t count = state.net.wifi_scan_count;
    if (count > WIFI_SCAN_MAX_RESULTS) count = WIFI_SCAN_MAX_RESULTS;

    if (event == TOUCH_EVENT_DOWN) {
        wifi_scan_dragging = false;
        wifi_scan_moved = false;
        wifi_scan_drag_start_y = ty;
        wifi_scan_last_y = ty;
        wifi_scan_last_interaction = millis();

        if (isHit(tx, ty, 350, 8, 110, 38)) {
            screens_set(SCREEN_WIFI_CONFIG);
            return;
        }

        if (isHit(tx, ty, 180, 278, 120, 36)) {
            wifi_scan_scroll_y = 0;
            wifi_scan_scroll_target = 0;
            data_lock(state);
            state.ui_needs_update = true;
            data_unlock(state);
            return;
        }

        if (isHit(tx, ty, 340, 278, 120, 36)) {
            wifi_scan_scroll_y = 0;
            wifi_scan_scroll_target = 0;
            if (ui_callbacks.onWiFiScan) ui_callbacks.onWiFiScan();
            return;
        }

        if (isHit(tx, ty, 20, WIFI_SCAN_LIST_TOP, 440, wifi_scan_view_h())) {
            wifi_scan_dragging = true;
        }
        return;
    }

    if (event == TOUCH_EVENT_MOVE && wifi_scan_dragging) {
        int dy = ty - wifi_scan_last_y;
        if (abs(ty - wifi_scan_drag_start_y) > 5) wifi_scan_moved = true;

        wifi_scan_scroll_target -= dy;
        wifi_scan_clamp_scroll(count);
        wifi_scan_last_y = ty;
        wifi_scan_last_interaction = millis();

        data_lock(state);
        state.ui_needs_update = true;
        data_unlock(state);
        return;
    }

    if (event == TOUCH_EVENT_UP && wifi_scan_dragging) {
        wifi_scan_dragging = false;
        wifi_scan_clamp_scroll(count);

        if (!wifi_scan_moved && isHit(tx, ty, 20, WIFI_SCAN_LIST_TOP, 440, wifi_scan_view_h())) {
            int content_y = ty - WIFI_SCAN_LIST_TOP + (int)wifi_scan_scroll_y;
            int index = content_y / WIFI_SCAN_ROW_H;

            if (index >= 0 && index < count) {
                strncpy(wifi_ssid, state.net.wifi_scan_results[index].ssid, sizeof(wifi_ssid) - 1);
                wifi_ssid[sizeof(wifi_ssid) - 1] = '\0';
                screens_set(SCREEN_WIFI_CONFIG);
            }
        }
    }
}

void handle_lan_config_touch(BuildingState& state, int tx, int ty) {
    if (isHit(tx, ty, 350, 8, 110, 38)) { screens_set(SCREEN_SETTINGS); }
    else if (isHit(tx, ty, 20, 58, 440, 50) || isHit(tx, ty, 180, 288, 120, 26)) {
        lan_dhcp = !lan_dhcp; 
        data_lock(state); state.ui_needs_update = true; data_unlock(state);
    }
    else if (isHit(tx, ty, 340, 288, 120, 26)) { 
        // SAVE
        data_lock(state);
        state.net.lan_use_dhcp = lan_dhcp;
        strncpy(state.net.lan_static_ip, temp_lan_ip, 15);
        strncpy(state.net.lan_gateway, temp_lan_gw, 15);
        strncpy(state.net.lan_subnet, temp_lan_sn, 15);
        strncpy(state.net.lan_dns, temp_lan_dns, 15);
        data_unlock(state);
        
        if (ui_callbacks.onLANSave) ui_callbacks.onLANSave();
        screens_set(SCREEN_SETTINGS); 
    }
    else if (!lan_dhcp) {
        if (isHit(tx, ty, 20, 176, 210, 48)) { editing_target = 3; keyboard_set_text(temp_lan_ip); screens_set(SCREEN_KEYBOARD); }
        else if (isHit(tx, ty, 250, 176, 210, 48)) { editing_target = 4; keyboard_set_text(temp_lan_gw); screens_set(SCREEN_KEYBOARD); }
        else if (isHit(tx, ty, 20, 232, 210, 48)) { editing_target = 5; keyboard_set_text(temp_lan_sn); screens_set(SCREEN_KEYBOARD); }
        else if (isHit(tx, ty, 250, 232, 210, 48)) { editing_target = 6; keyboard_set_text(temp_lan_dns); screens_set(SCREEN_KEYBOARD); }
    }
}

void handle_slave_manager_touch(BuildingState& state, int tx, int ty) {
    uint8_t visible_indices[RS485_MAX_SLAVES] = {0};
    uint8_t count = slave_manager_build_indices(state.rs485, visible_indices, RS485_MAX_SLAVES);
    bool empty_list = slave_manager_is_empty(visible_indices, count);
    uint8_t total_pages = (count + SLAVE_PAGE_ROWS - 1) / SLAVE_PAGE_ROWS;
    if (total_pages == 0) total_pages = 1;
    if (slave_current_page >= total_pages) slave_current_page = total_pages - 1;
    if (!slave_manager_selection_visible(visible_indices, count)) {
        slave_selected_index = visible_indices[0] == SLAVE_PLACEHOLDER_INDEX ? 0 : visible_indices[0];
    }

    if (isHit(tx, ty, 350, 8, 110, 38) || isHit(tx, ty, 15, 292, 90, 24)) {
        screens_set(SCREEN_SETTINGS);
        return;
    }

    if ((empty_list || state.rs485.pairing_active || state.rs485.pairing_candidate_ready) &&
        isHit(tx, ty, 38, 146, 404, 132)) {
        if (state.rs485.pairing_candidate_ready) {
            if (ui_callbacks.onRS485PairingAssign) ui_callbacks.onRS485PairingAssign(0);
        } else if (!state.rs485.pairing_active) {
            if (ui_callbacks.onRS485Pairing) ui_callbacks.onRS485Pairing();
        }
        data_lock(state);
        state.ui_needs_update = true;
        data_unlock(state);
        return;
    }

    if (total_pages > 1 && isHit(tx, ty, 375, 292, 90, 24)) {
        slave_current_page = (slave_current_page + 1) % total_pages;
        data_lock(state);
        state.ui_needs_update = true;
        data_unlock(state);
        return;
    }

    if (isHit(tx, ty, 20, 104, 112, 38)) {
        if (state.rs485.pairing_active) {
            if (ui_callbacks.onRS485PairingCancel) ui_callbacks.onRS485PairingCancel();
        } else if (ui_callbacks.onRS485Pairing) {
            ui_callbacks.onRS485Pairing();
        }
        data_lock(state);
        state.ui_needs_update = true;
        data_unlock(state);
        return;
    }

    if (isHit(tx, ty, 144, 104, 104, 38)) {
        if (ui_callbacks.onRS485PollToggle) ui_callbacks.onRS485PollToggle(!state.rs485.poll_enabled);
        data_lock(state);
        state.ui_needs_update = true;
        data_unlock(state);
        return;
    }

    if (state.rs485.pairing_candidate_ready && isHit(tx, ty, 260, 104, 200, 38)) {
        if (ui_callbacks.onRS485PairingAssign) ui_callbacks.onRS485PairingAssign(0);
        return;
    }
}

void handle_slave_manager_touch_event(BuildingState& state, int tx, int ty, TouchEventType event) {
    uint8_t visible_indices[RS485_MAX_SLAVES] = {0};
    uint8_t count = slave_manager_build_indices(state.rs485, visible_indices, RS485_MAX_SLAVES);
    bool empty_list = slave_manager_is_empty(visible_indices, count);
    uint8_t total_pages = (count + SLAVE_PAGE_ROWS - 1) / SLAVE_PAGE_ROWS;
    if (total_pages == 0) total_pages = 1;

    if (event == TOUCH_EVENT_DOWN) {
        slave_dragging = false;
        slave_moved = false;
        slave_drag_start_y = ty;
        slave_last_y = ty;

        if (isHit(tx, ty, 350, 8, 110, 38) ||
            isHit(tx, ty, 20, 104, 112, 38) ||
            isHit(tx, ty, 144, 104, 104, 38) ||
            isHit(tx, ty, 260, 104, 200, 38) ||
            isHit(tx, ty, 15, 292, 90, 24) ||
            isHit(tx, ty, 375, 292, 90, 24) ||
            ((empty_list || state.rs485.pairing_active || state.rs485.pairing_candidate_ready) &&
             isHit(tx, ty, 38, 146, 404, 132))) {
            handle_slave_manager_touch(state, tx, ty);
            return;
        }

        if (empty_list || state.rs485.pairing_active || state.rs485.pairing_candidate_ready) {
            return;
        }

        if (isHit(tx, ty, 15, SLAVE_LIST_TOP, 450, slave_list_view_h())) {
            slave_dragging = true;
        }
        return;
    }

    if (event == TOUCH_EVENT_MOVE && slave_dragging) {
        int dy = ty - slave_last_y;
        if (abs(ty - slave_drag_start_y) > 5) slave_moved = true;

        slave_scroll_target -= dy;
        slave_list_clamp_scroll(count);
        slave_last_y = ty;

        data_lock(state);
        state.ui_needs_update = true;
        data_unlock(state);
        return;
    }

    if (event == TOUCH_EVENT_UP && slave_dragging) {
        slave_dragging = false;
        if (!slave_moved && isHit(tx, ty, 15, SLAVE_LIST_TOP, 450, slave_list_view_h())) {
            int row_on_page = (ty - SLAVE_LIST_TOP) / SLAVE_ROW_H;
            int index = slave_current_page * SLAVE_PAGE_ROWS + row_on_page;

            if (index >= 0 && index < count) {
                uint8_t slave_index = visible_indices[index];
                if (slave_index == SLAVE_PLACEHOLDER_INDEX) {
                    slave_detail_dummy = true;
                    slave_selected_index = 0;
                    slave_feature_page = 0;
                    screens_set(SCREEN_SLAVE_DETAIL);
                    return;
                }

                if (slave_selected_index == slave_index) {
                    slave_detail_dummy = false;
                    slave_feature_page = 0;
                    screens_set(SCREEN_SLAVE_DETAIL);
                } else {
                    slave_selected_index = slave_index;
                    data_lock(state);
                    state.ui_needs_update = true;
                    data_unlock(state);
                }
            }
        }
    }
}

void handle_slave_detail_touch(BuildingState& state, int tx, int ty) {
    uint8_t target_index = slave_detail_dummy ? 0 : (uint8_t)slave_selected_index;
    if (target_index >= RS485_MAX_SLAVES) return;

    if (isHit(tx, ty, 372, 8, 90, 34)) {
        screens_set(SCREEN_SLAVE_MANAGER);
        return;
    }

    RS485SlaveState page_slave = {};
    data_lock(state);
    if (target_index < RS485_MAX_SLAVES) page_slave = state.rs485.slaves[target_index];
    data_unlock(state);

    if (!slave_detail_dummy && (page_slave.address != 0 || page_slave.uid != 0) &&
        isHit(tx, ty, 254, 66, 84, 38)) {
        data_lock(state);
        slave_forget_locked(state, target_index);
        slave_selected_index = 0;
        slave_feature_page = 0;
        data_unlock(state);
        data_save_rs485_config(state);
        screens_set(SCREEN_SLAVE_MANAGER);
        return;
    }

    if (isHit(tx, ty, 350, 66, 96, 38) || isHit(tx, ty, 20, 56, 224, 58)) {
        editing_target = 7;
        keyboard_set_text(state.rs485.slaves[target_index].name);
        screens_set(SCREEN_KEYBOARD);
        return;
    }

    uint8_t total_pages = slave_feature_total_pages_for_state(page_slave);
    if (slave_feature_page >= total_pages) slave_feature_page = total_pages - 1;
    if (slave_feature_page < 0) slave_feature_page = 0;
    if (total_pages > 1 && isHit(tx, ty, 144, 286, 190, 28)) {
        slave_feature_page = (slave_feature_page + 1) % total_pages;
        data_lock(state);
        state.ui_needs_update = true;
        data_unlock(state);
        return;
    }

    uint8_t first_feature = slave_feature_page * SLAVE_FEATURE_PAGE_ROWS;
    uint8_t total_features = slave_feature_visible_count_for_state(page_slave);
    for (uint8_t row = 0; row < SLAVE_FEATURE_PAGE_ROWS; row++) {
        uint8_t visible_index = first_feature + row;
        if (visible_index >= total_features) break;
        const FeatureRow* feature_ptr = slave_feature_visible_at(page_slave, visible_index);
        if (!feature_ptr) break;
        const FeatureRow& feature = *feature_ptr;
        int y = 190 + row * 31;
        if (!isHit(tx, ty, 20, y, 440, 28)) continue;

        data_lock(state);
        RS485SlaveState& slave = state.rs485.slaves[target_index];
        bool available = slave_feature_available_for_state(state.rs485, target_index, slave, feature);
        bool enabled = slave_feature_enabled(slave, feature);
        if (available) {
            if (feature.profile_row) {
                if (enabled) {
                    slave_clear_profile_selection(slave);
                } else {
                    slave_apply_profile_policy_for_state(state.rs485, target_index, slave, feature.profile, true);
                }
                slave_feature_page = 0;
            } else if (feature.capability == RS485_CAP_TEMP) {
                slave_mark_feature_assignable(slave, feature);
                slave.enabled_mask |= RS485_CAP_TEMP;
                bool next_enabled = !(slave.temp_enabled_mask & (1 << feature.channel));
                if (next_enabled) {
                    uint8_t count = state.rs485.slave_count;
                    if (count > RS485_MAX_SLAVES) count = RS485_MAX_SLAVES;
                    for (uint8_t other = 0; other < count; other++) {
                        if (other == target_index) continue;
                        state.rs485.slaves[other].temp_enabled_mask &= ~(1 << feature.channel);
                        if ((state.rs485.slaves[other].temp_enabled_mask & 0x0F) == 0) {
                            state.rs485.slaves[other].enabled_mask &= ~RS485_CAP_TEMP;
                        }
                    }
                    slave.temp_enabled_mask |= (1 << feature.channel);
                } else {
                    slave.temp_enabled_mask &= ~(1 << feature.channel);
                }
                if ((slave.temp_enabled_mask & 0x0F) == 0) {
                    slave.enabled_mask &= ~RS485_CAP_TEMP;
                }
            } else if (slave.enabled_mask & feature.capability) {
                slave.enabled_mask &= ~feature.capability;
            } else {
                slave_mark_feature_assignable(slave, feature);
                slave.enabled_mask |= feature.capability;
            }
            mapping_manager_update_locked(state);
            state.ui_needs_update = true;
        }
        data_unlock(state);
        return;
    }

    if (isHit(tx, ty, 356, 286, 104, 28)) {
        data_lock(state);
        mapping_manager_update_locked(state);
        state.ui_needs_update = true;
        data_unlock(state);
        data_save_rs485_config(state);
        rs485_apply_slave_assignments(target_index);
        screens_set(SCREEN_SLAVE_MANAGER);
        return;
    }
}

void handle_dashboard_mapping_touch(BuildingState& state, int tx, int ty) {
    uint8_t total_pages = mapping_total_pages();

    uint8_t first = mapping_page * SLAVE_FEATURE_PAGE_ROWS;
    for (uint8_t row = 0; row < SLAVE_FEATURE_PAGE_ROWS; row++) {
        uint8_t slot = first + row;
        if (slot >= DASHBOARD_LOGICAL_SLOT_COUNT) break;
        int y = 84 + row * 62;
        if (isHit(tx, ty, 20, y, 440, 54)) {
            mapping_selected_slot = slot;
            mapping_source_page = 0;
            screens_set(SCREEN_MAPPING_SOURCE);
            return;
        }
    }

    if (isHit(tx, ty, 372, 8, 90, 34) || isHit(tx, ty, 20, 286, 118, 28)) {
        screens_set(SCREEN_SLAVE_DETAIL);
        return;
    }

    if (isHit(tx, ty, 170, 286, 140, 28)) {
        mapping_page = (mapping_page + 1) % total_pages;
        data_lock(state);
        state.ui_needs_update = true;
        data_unlock(state);
        return;
    }

    if (isHit(tx, ty, 342, 286, 118, 28)) {
        data_lock(state);
        for (uint8_t i = 0; i < DASHBOARD_LOGICAL_SLOT_COUNT; i++) {
            state.rs485.mappings[i].assigned = false;
            state.rs485.mappings[i].manual_override = false;
            state.rs485.mappings[i].slave_uid = 0;
            state.rs485.mappings[i].slave_addr = 0;
            state.rs485.mappings[i].channel = 0;
        }
        mapping_manager_update_locked(state);
        state.ui_needs_update = true;
        data_unlock(state);
        data_save_rs485_config(state);
        return;
    }
}

void handle_mapping_source_touch(BuildingState& state, int tx, int ty) {
    if (mapping_selected_slot < 0) mapping_selected_slot = 0;
    if (mapping_selected_slot >= DASHBOARD_LOGICAL_SLOT_COUNT) mapping_selected_slot = DASHBOARD_LOGICAL_SLOT_COUNT - 1;

    LogicalMapping& selected = state.rs485.mappings[mapping_selected_slot];
    uint16_t capability = selected.capability_type;
    uint8_t option_count = mapping_source_option_count(state.rs485, capability);
    uint8_t total_pages = (option_count + SLAVE_FEATURE_PAGE_ROWS - 1) / SLAVE_FEATURE_PAGE_ROWS;
    if (total_pages == 0) total_pages = 1;

    if (isHit(tx, ty, 372, 8, 90, 34) || isHit(tx, ty, 20, 286, 118, 28)) {
        screens_set(SCREEN_DASHBOARD_MAPPING);
        return;
    }

    if (isHit(tx, ty, 342, 286, 118, 28) ||
        (total_pages > 1 && isHit(tx, ty, 170, 286, 140, 28))) {
        mapping_source_page = (mapping_source_page + 1) % total_pages;
        data_lock(state);
        state.ui_needs_update = true;
        data_unlock(state);
        return;
    }

    uint8_t first = mapping_source_page * SLAVE_FEATURE_PAGE_ROWS;
    for (uint8_t row = 0; row < SLAVE_FEATURE_PAGE_ROWS; row++) {
        uint8_t option = first + row;
        if (option >= option_count) break;
        int y = 84 + row * 62;
        if (!isHit(tx, ty, 20, y, 440, 54)) continue;

        data_lock(state);
        LogicalMapping& mapping = state.rs485.mappings[mapping_selected_slot];
        if (option == 0) {
            mapping.assigned = false;
            mapping.manual_override = true;
            mapping.slave_uid = 0;
            mapping.slave_addr = 0;
            mapping.channel = 0;
        } else {
            uint8_t slave_index = 0;
            uint8_t channel = 0;
            if (mapping_source_option_at(state.rs485, capability, option, &slave_index, &channel)) {
                const RS485SlaveState& slave = state.rs485.slaves[slave_index];
                mapping.slave_uid = slave.uid;
                mapping.slave_addr = slave.address;
                mapping.channel = channel;
                mapping.assigned = true;
                mapping.manual_override = true;
            }
        }
        mapping_manager_update_locked(state);
        state.ui_needs_update = true;
        data_unlock(state);
        data_save_rs485_config(state);
        screens_set(SCREEN_DASHBOARD_MAPPING);
        return;
    }
}

void handle_keyboard_touch(BuildingState& state, int tx, int ty) {
    int kY_row4 = 243; // Sesuai kY Row 4 di ui_keyboard.cpp
    
    // OK / ENTER (Pindah ke kanan: 340, 130)
    if (isHit(tx, ty, 340, kY_row4, 130, 45)) {
        if (editing_target == 1) {
            strncpy(wifi_ssid, keyboard_get_text(), sizeof(wifi_ssid) - 1);
            wifi_ssid[sizeof(wifi_ssid) - 1] = '\0';
        }
        else if (editing_target == 2) {
            strncpy(wifi_pass, keyboard_get_text(), sizeof(wifi_pass) - 1);
            wifi_pass[sizeof(wifi_pass) - 1] = '\0';
        }
        else if (editing_target == 3) strncpy(temp_lan_ip, keyboard_get_text(), 15);
        else if (editing_target == 4) strncpy(temp_lan_gw, keyboard_get_text(), 15);
        else if (editing_target == 5) strncpy(temp_lan_sn, keyboard_get_text(), 15);
        else if (editing_target == 6) strncpy(temp_lan_dns, keyboard_get_text(), 15);
        else if (editing_target == 7) {
            data_lock(state);
            uint8_t target_index = slave_detail_dummy ? 0 : (uint8_t)slave_selected_index;
            uint8_t count = state.rs485.slave_count;
            if (count > RS485_MAX_SLAVES) count = RS485_MAX_SLAVES;
            if (target_index < count) {
                strncpy(state.rs485.slaves[target_index].name, keyboard_get_text(),
                        sizeof(state.rs485.slaves[target_index].name) - 1);
                state.rs485.slaves[target_index].name[sizeof(state.rs485.slaves[target_index].name) - 1] = '\0';
                state.ui_needs_update = true;
            }
            data_unlock(state);
            data_save_rs485_config(state);
        }
        else if (editing_target == 8) {
            data_lock(state);
            strncpy(state.net.device_name, keyboard_get_text(), sizeof(state.net.device_name) - 1);
            state.net.device_name[sizeof(state.net.device_name) - 1] = '\0';
            state.ui_needs_update = true;
            data_unlock(state);
            data_save_device_config(state);
        }
        else if (editing_target == 9) {
            data_lock(state);
            strncpy(state.net.class_name, keyboard_get_text(), sizeof(state.net.class_name) - 1);
            state.net.class_name[sizeof(state.net.class_name) - 1] = '\0';
            if (state.net.class_name[0] == '\0') {
                strncpy(state.net.class_name, "HD01", sizeof(state.net.class_name) - 1);
                state.net.class_name[sizeof(state.net.class_name) - 1] = '\0';
            }
            state.ui_needs_update = true;
            data_unlock(state);
            data_save_device_config(state);
        }
        else if (editing_target == 10) {
            data_lock(state);
            strncpy(state.net.mqtt_server, keyboard_get_text(), sizeof(state.net.mqtt_server) - 1);
            state.net.mqtt_server[sizeof(state.net.mqtt_server) - 1] = '\0';
            state.ui_needs_update = true;
            data_unlock(state);
            data_save_device_config(state);
        }
        else if (editing_target == 11) {
            uint32_t port = strtoul(keyboard_get_text(), nullptr, 10);
            if (port == 0 || port > 65535) port = state.net.mqtt_use_tls ? 8883 : 1883;
            data_lock(state);
            state.net.mqtt_port = (uint16_t)port;
            state.ui_needs_update = true;
            data_unlock(state);
            data_save_device_config(state);
        }
        else if (editing_target == 12) {
            data_lock(state);
            strncpy(state.net.mqtt_user, keyboard_get_text(), sizeof(state.net.mqtt_user) - 1);
            state.net.mqtt_user[sizeof(state.net.mqtt_user) - 1] = '\0';
            state.ui_needs_update = true;
            data_unlock(state);
            data_save_device_config(state);
        }
        else if (editing_target == 13) {
            data_lock(state);
            strncpy(state.net.mqtt_pass, keyboard_get_text(), sizeof(state.net.mqtt_pass) - 1);
            state.net.mqtt_pass[sizeof(state.net.mqtt_pass) - 1] = '\0';
            state.ui_needs_update = true;
            data_unlock(state);
            data_save_device_config(state);
        }
        
        screens_set((editing_target == 1 || editing_target == 2) ? SCREEN_WIFI_CONFIG : 
                    (editing_target >= 3 && editing_target <= 6) ? SCREEN_LAN_CONFIG :
                    (editing_target == 7) ? SCREEN_SLAVE_DETAIL :
                    (editing_target >= 10 && editing_target <= 13) ? SCREEN_MQTT_SETUP : SCREEN_SETTINGS);
        editing_target = 0;
        return;
    }
    
    // BACK / CANCEL (Pindah ke tengah-kanan: 250, 80)
    if (isHit(tx, ty, 250, kY_row4, 80, 45)) {
        screens_set((editing_target == 1 || editing_target == 2) ? SCREEN_WIFI_CONFIG : 
                    (editing_target >= 3 && editing_target <= 6) ? SCREEN_LAN_CONFIG :
                    (editing_target == 7) ? SCREEN_SLAVE_DETAIL :
                    (editing_target >= 10 && editing_target <= 13) ? SCREEN_MQTT_SETUP : SCREEN_SETTINGS);
        editing_target = 0;
        return;
    }
    
    // Serahkan sisanya (123 mode, huruf, angka, space) ke internal keyboard_handle_touch
    keyboard_handle_touch(tx, ty);
}

void screens_handle_touch(BuildingState& state, int tx, int ty) {
    switch (current_screen) {
        case SCREEN_DASHBOARD:   handle_dashboard_touch(state, tx, ty);   break;
        case SCREEN_SETTINGS:    /* Handled by handle_settings_touch_event */  break;
        case SCREEN_WIFI_CONFIG: handle_wifi_touch(state, tx, ty);        break;
        case SCREEN_LAN_CONFIG:  handle_lan_config_touch(state, tx, ty);  break;
        case SCREEN_WIFI_SCAN:   handle_wifi_scan_touch(state, tx, ty);   break;
        case SCREEN_SLAVE_MANAGER: handle_slave_manager_touch(state, tx, ty); break;
        case SCREEN_SLAVE_DETAIL: handle_slave_detail_touch(state, tx, ty); break;
        case SCREEN_DASHBOARD_MAPPING: handle_dashboard_mapping_touch(state, tx, ty); break;
        case SCREEN_MAPPING_SOURCE: handle_mapping_source_touch(state, tx, ty); break;
        case SCREEN_TEMP_DETAIL: handle_temperature_detail_touch(state, tx, ty); break;
        case SCREEN_DEVICE_INFO: handle_device_info_touch(state, tx, ty); break;
        case SCREEN_MQTT_SETUP: handle_mqtt_setup_touch(state, tx, ty); break;
        case SCREEN_CLOCK_SETUP: handle_clock_setup_touch(state, tx, ty); break;
        case SCREEN_TOUCH_TEST: break;
        case SCREEN_KEYBOARD:    handle_keyboard_touch(state, tx, ty);    break;
        default: break;
    }
}

void screens_handle_touch_event(BuildingState& state, int tx, int ty, TouchEventType event) {
    if (current_screen == SCREEN_DASHBOARD) {
        handle_dashboard_touch_event(state, tx, ty, event);
        return;
    }

    if (current_screen == SCREEN_WIFI_SCAN) {
        handle_wifi_scan_touch_event(state, tx, ty, event);
        return;
    }

    if (current_screen == SCREEN_SLAVE_MANAGER) {
        handle_slave_manager_touch_event(state, tx, ty, event);
        return;
    }

    if (current_screen == SCREEN_SETTINGS) {
        handle_settings_touch_event(state, tx, ty, event);
        return;
    }

    if (current_screen == SCREEN_CLOCK_SETUP) {
        if (event == TOUCH_EVENT_DOWN) {
            handle_clock_setup_touch(state, tx, ty);
        }
        return;
    }

    if (event == TOUCH_EVENT_DOWN) {
        screens_handle_touch(state, tx, ty);
    }
}

static int get_days_in_month(int year, int month) {
    static const int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12) return 31;
    if (month == 2) {
        bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return days[month - 1];
}

void render_clock_setup(BuildingState& state) {
    drawWallpaperBackground();

    p_canvas->setTextDatum(TextDatum::TopLeft);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->setTextFont(4);
    p_canvas->drawString("Clock Setup", 20, 10);

    // BACK button
    drawCardBase(338, 8, 122, 38, COLOR_STAT_OFF);
    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString("BACK", 399, 27);

    // Left side panel
    // Mode Switch Button (NTP / MANUAL)
    uint16_t mode_color = clock_setup_manual_mode ? COLOR_STAT_WARN : COLOR_STAT_ON;
    drawCardBase(20, 58, 180, 48, COLOR_CARD_BG);
    p_canvas->fillRoundRect(34, 58 + 10, 6, 28, 3, mode_color);
    p_canvas->setTextDatum(TextDatum::MiddleLeft);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString("MODE", 52, 72);
    p_canvas->setTextFont(4);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString(clock_setup_manual_mode ? "MANUAL" : "NTP", 52, 92);

    // CANCEL button
    drawCardBase(20, 126, 180, 48, COLOR_CARD_BG);
    p_canvas->fillRoundRect(34, 126 + 10, 6, 28, 3, COLOR_STAT_OFF);
    p_canvas->setTextDatum(TextDatum::MiddleLeft);
    p_canvas->setTextFont(4);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString("CANCEL", 52, 150);

    // SAVE & APPLY button
    drawCardBase(20, 194, 180, 48, COLOR_CARD_BG);
    p_canvas->fillRoundRect(34, 194 + 10, 6, 28, 3, COLOR_STAT_ON);
    p_canvas->setTextDatum(TextDatum::MiddleLeft);
    p_canvas->setTextFont(4);
    p_canvas->setTextColor(COLOR_TEXT_MAIN);
    p_canvas->drawString("SAVE", 52, 218);

    // Status message at the bottom left
    p_canvas->setTextDatum(TextDatum::MiddleLeft);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString(clock_setup_manual_mode ? "Set clock manually" : "NTP time sync active", 20, 270);

    // Right side: adjustment columns
    drawCardBase(220, 58, 240, 180, COLOR_CARD_BG);

    uint16_t text_color = clock_setup_manual_mode ? COLOR_TEXT_MAIN : COLOR_TEXT_SEC;
    uint16_t button_text_color = clock_setup_manual_mode ? COLOR_ACCENT_MAIN : COLOR_STAT_OFF;

    char buf[8];

    // Day column
    if (clock_setup_manual_mode) {
        drawCardBase(235, 70, 30, 24, COLOR_BG_MAIN);
    }
    p_canvas->setTextDatum(TextDatum::MiddleCenter);
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(button_text_color);
    p_canvas->drawString("+", 250, 82);
    p_canvas->setTextFont(4);
    p_canvas->setTextColor(text_color);
    snprintf(buf, sizeof(buf), "%02d", manual_clock_day);
    p_canvas->drawString(buf, 250, 119);
    if (clock_setup_manual_mode) {
        drawCardBase(235, 144, 30, 24, COLOR_BG_MAIN);
    }
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(button_text_color);
    p_canvas->drawString("-", 250, 156);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString("Day", 250, 192);

    // Month column
    if (clock_setup_manual_mode) {
        drawCardBase(275, 70, 30, 24, COLOR_BG_MAIN);
    }
    p_canvas->setTextColor(button_text_color);
    p_canvas->drawString("+", 290, 82);
    p_canvas->setTextFont(4);
    p_canvas->setTextColor(text_color);
    snprintf(buf, sizeof(buf), "%02d", manual_clock_month);
    p_canvas->drawString(buf, 290, 119);
    if (clock_setup_manual_mode) {
        drawCardBase(275, 144, 30, 24, COLOR_BG_MAIN);
    }
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(button_text_color);
    p_canvas->drawString("-", 290, 156);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString("Mon", 290, 192);

    // Year column
    if (clock_setup_manual_mode) {
        drawCardBase(320, 70, 40, 24, COLOR_BG_MAIN);
    }
    p_canvas->setTextColor(button_text_color);
    p_canvas->drawString("+", 340, 82);
    p_canvas->setTextFont(4);
    p_canvas->setTextColor(text_color);
    snprintf(buf, sizeof(buf), "%04d", manual_clock_year);
    p_canvas->drawString(buf, 340, 119);
    if (clock_setup_manual_mode) {
        drawCardBase(320, 144, 40, 24, COLOR_BG_MAIN);
    }
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(button_text_color);
    p_canvas->drawString("-", 340, 156);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString("Year", 340, 192);

    // Hour column
    if (clock_setup_manual_mode) {
        drawCardBase(375, 70, 30, 24, COLOR_BG_MAIN);
    }
    p_canvas->setTextColor(button_text_color);
    p_canvas->drawString("+", 390, 82);
    p_canvas->setTextFont(4);
    p_canvas->setTextColor(text_color);
    snprintf(buf, sizeof(buf), "%02d", manual_clock_hour);
    p_canvas->drawString(buf, 390, 119);
    if (clock_setup_manual_mode) {
        drawCardBase(375, 144, 30, 24, COLOR_BG_MAIN);
    }
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(button_text_color);
    p_canvas->drawString("-", 390, 156);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString("Hour", 390, 192);

    // Minute column
    if (clock_setup_manual_mode) {
        drawCardBase(415, 70, 30, 24, COLOR_BG_MAIN);
    }
    p_canvas->setTextColor(button_text_color);
    p_canvas->drawString("+", 430, 82);
    p_canvas->setTextFont(4);
    p_canvas->setTextColor(text_color);
    snprintf(buf, sizeof(buf), "%02d", manual_clock_minute);
    p_canvas->drawString(buf, 430, 119);
    if (clock_setup_manual_mode) {
        drawCardBase(415, 144, 30, 24, COLOR_BG_MAIN);
    }
    p_canvas->setTextFont(2);
    p_canvas->setTextColor(button_text_color);
    p_canvas->drawString("-", 430, 156);
    p_canvas->setTextColor(COLOR_TEXT_SEC);
    p_canvas->drawString("Min", 430, 192);

    p_canvas->setTextDatum(TextDatum::TopLeft);
}

void handle_clock_setup_touch(BuildingState& state, int tx, int ty) {
    if (isHit(tx, ty, 338, 8, 122, 38) || isHit(tx, ty, 20, 126, 180, 48)) {
        screens_set(SCREEN_SETTINGS);
        return;
    }

    if (isHit(tx, ty, 20, 58, 180, 48)) {
        clock_setup_manual_mode = !clock_setup_manual_mode;
        data_lock(state);
        state.ui_needs_update = true;
        data_unlock(state);
        return;
    }

    if (isHit(tx, ty, 20, 194, 180, 48)) {
        data_lock(state);
        state.net.use_manual_time = clock_setup_manual_mode;
        state.ui_needs_update = true;
        data_unlock(state);
        data_save_device_config(state);

        if (clock_setup_manual_mode) {
            time_manager_set_manual(manual_clock_year, manual_clock_month, manual_clock_day, manual_clock_hour, manual_clock_minute);
        }
        screens_set(SCREEN_SETTINGS);
        return;
    }

    if (clock_setup_manual_mode) {
        bool changed = false;

        // Day Plus: 235, 70, 30, 24
        if (isHit(tx, ty, 235, 70, 30, 24)) {
            manual_clock_day++;
            int max_days = get_days_in_month(manual_clock_year, manual_clock_month);
            if (manual_clock_day > max_days) manual_clock_day = 1;
            changed = true;
        }
        // Day Minus: 235, 144, 30, 24
        else if (isHit(tx, ty, 235, 144, 30, 24)) {
            manual_clock_day--;
            int max_days = get_days_in_month(manual_clock_year, manual_clock_month);
            if (manual_clock_day < 1) manual_clock_day = max_days;
            changed = true;
        }

        // Month Plus: 275, 70, 30, 24
        else if (isHit(tx, ty, 275, 70, 30, 24)) {
            manual_clock_month++;
            if (manual_clock_month > 12) manual_clock_month = 1;
            int max_days = get_days_in_month(manual_clock_year, manual_clock_month);
            if (manual_clock_day > max_days) manual_clock_day = max_days;
            changed = true;
        }
        // Month Minus: 275, 144, 30, 24
        else if (isHit(tx, ty, 275, 144, 30, 24)) {
            manual_clock_month--;
            if (manual_clock_month < 1) manual_clock_month = 12;
            int max_days = get_days_in_month(manual_clock_year, manual_clock_month);
            if (manual_clock_day > max_days) manual_clock_day = max_days;
            changed = true;
        }

        // Year Plus: 320, 70, 40, 24
        else if (isHit(tx, ty, 320, 70, 40, 24)) {
            manual_clock_year++;
            if (manual_clock_year > 2099) manual_clock_year = 2024;
            int max_days = get_days_in_month(manual_clock_year, manual_clock_month);
            if (manual_clock_day > max_days) manual_clock_day = max_days;
            changed = true;
        }
        // Year Minus: 320, 144, 40, 24
        else if (isHit(tx, ty, 320, 144, 40, 24)) {
            manual_clock_year--;
            if (manual_clock_year < 2024) manual_clock_year = 2099;
            int max_days = get_days_in_month(manual_clock_year, manual_clock_month);
            if (manual_clock_day > max_days) manual_clock_day = max_days;
            changed = true;
        }

        // Hour Plus: 375, 70, 30, 24
        else if (isHit(tx, ty, 375, 70, 30, 24)) {
            manual_clock_hour++;
            if (manual_clock_hour > 23) manual_clock_hour = 0;
            changed = true;
        }
        // Hour Minus: 375, 144, 30, 24
        else if (isHit(tx, ty, 375, 144, 30, 24)) {
            manual_clock_hour--;
            if (manual_clock_hour < 0) manual_clock_hour = 23;
            changed = true;
        }

        // Minute Plus: 415, 70, 30, 24
        else if (isHit(tx, ty, 415, 70, 30, 24)) {
            manual_clock_minute++;
            if (manual_clock_minute > 59) manual_clock_minute = 0;
            changed = true;
        }
        // Minute Minus: 415, 144, 30, 24
        else if (isHit(tx, ty, 415, 144, 30, 24)) {
            manual_clock_minute--;
            if (manual_clock_minute < 0) manual_clock_minute = 59;
            changed = true;
        }

        if (changed) {
            data_lock(state);
            state.ui_needs_update = true;
            data_unlock(state);
        }
    }
}

