#ifndef TIME_MANAGER_H
#define TIME_MANAGER_H

#include <Arduino.h>
#include <time.h>
#include <WiFi.h>
#include <Ethernet.h>
#include <EthernetUdp.h>

void time_manager_init();
void time_manager_update();
void time_manager_set_manual(int year, int month, int day, int hour, int minute);

#endif
