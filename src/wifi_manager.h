#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>

void wifi_manager_init();
void wifi_manager_connect(const char* ssid, const char* pass);
void wifi_manager_load_and_connect();
void wifi_manager_loop();
void wifi_manager_set_power(bool on);
void wifi_manager_reconnect();
void wifi_manager_scan_request();

#endif
