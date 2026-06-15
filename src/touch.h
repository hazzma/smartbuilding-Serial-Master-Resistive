#ifndef TOUCH_H
#define TOUCH_H

#include <Arduino.h>

enum TouchEventType {
    TOUCH_EVENT_NONE = 0,
    TOUCH_EVENT_DOWN,
    TOUCH_EVENT_MOVE,
    TOUCH_EVENT_UP
};

void touch_init();
bool touch_get_point(int &tx, int &ty);
bool touch_get_event(int &tx, int &ty, TouchEventType &event);

#endif
