#ifndef UI_SCREENS_H
#define UI_SCREENS_H

#include "data.h"
#include "touch.h"

enum ScreenState {
    SCREEN_DASHBOARD    = 0,
    SCREEN_SETTINGS     = 1,
    SCREEN_WIFI_CONFIG  = 2,
    SCREEN_KEYBOARD     = 3,
    SCREEN_LAN_STATUS   = 4,
    SCREEN_LAN_CONFIG   = 5,
    SCREEN_SLAVE_MANAGER = 6,
    SCREEN_WIFI_SCAN    = 8,
    SCREEN_TEMP_DETAIL  = 9,
    SCREEN_SLAVE_DETAIL = 10,
    SCREEN_DASHBOARD_MAPPING = 11,
    SCREEN_MAPPING_SOURCE = 12,
    SCREEN_DEVICE_INFO = 13,
    SCREEN_MQTT_SETUP = 14,
    SCREEN_TOUCH_TEST = 15,
    SCREEN_CLOCK_SETUP = 16
};

struct UIEventCallbacks {
    void (*onWiFiConnect)(const char* ssid, const char* pass);
    void (*onWiFiReconnect)();
    void (*onWiFiScan)();
    void (*onLANSave)();
    void (*onPriorityChange)(int priority);
    void (*onRS485Pairing)();
    void (*onRS485PairingCancel)();
    void (*onRS485PairingAssign)(uint8_t address);
    void (*onRS485PollToggle)(bool enabled);
    void (*onRS485Test)(uint8_t address, uint8_t cmd, bool write_command);
};

void screens_init(UIEventCallbacks callbacks);
void screens_render(BuildingState& state, int fps);
void screens_set(ScreenState s);
void screens_handle_touch(BuildingState& state, int tx, int ty);
void screens_handle_touch_event(BuildingState& state, int tx, int ty, TouchEventType event);
bool screens_has_animation();

#endif
