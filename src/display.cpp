#include "display.h"

LGFX tft;
SemaphoreHandle_t bus_mutex = NULL;

void display_init() {
    bus_mutex = xSemaphoreCreateMutex();

    // GPIO46 is a boot strapping pin. Keep touch deselected before SPI init.
    pinMode(TOUCH_CS, OUTPUT);
    digitalWrite(TOUCH_CS, HIGH);

    tft.init();
    tft.setRotation(1);      
    tft.invertDisplay(false);
    tft.fillScreen(COLOR_BG_MAIN);
    tft.setBrightness(200);   // ~78% brightness on boot

    Serial.println("[DISP] LovyanGFX ILI9488 initialized (480x320 SPI)");
}

void display_brightness(uint8_t pct) {
    // pct: 0-100
    uint8_t val = (uint8_t)((float)pct / 100.0f * 255.0f);
    tft.setBrightness(val);
}
