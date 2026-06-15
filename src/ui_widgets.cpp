#include "ui_widgets.h"
#include "data.h"
#include "lgfx_adapter.h"
#include "wallpaper_asset.h"
#include "settings_logo_asset.h"
#include <pgmspace.h>
#include <stdio.h>

// ── Display Engine Implementation (LovyanGFX) ────────────────────────────────
static LGFXAdapter lgfx_engine(&tft);
DisplayEngine* p_engine = &lgfx_engine;

static void drawSettingsLogoIcon(int x, int y, int size) {
    if (size <= 0) return;
    for (int dy = 0; dy < size; dy++) {
        int sy = (dy * SETTINGS_LOGO_HEIGHT) / size;
        for (int dx = 0; dx < size; dx++) {
            int sx = (dx * SETTINGS_LOGO_WIDTH) / size;
            uint16_t color = pgm_read_word(&epd_bitmap_Code_Generated_Image__1_[sy * SETTINGS_LOGO_WIDTH + sx]);
            if (color >= 0xffdf) continue;
            p_engine->fillRect(x + dx, y + dy, 1, 1, color);
        }
    }
}

void widgets_init() {
    Serial.println("\n[HAL] --- Display Engine Initialization (LovyanGFX) ---");

    if (p_engine->init(480, 320)) {
        Serial.println("[HAL] Engine initialized successfully ✓");
    } else {
        Serial.println("[HAL] Engine initialization FAILED!");
    }

    Serial.printf("[MEM] Free PSRAM: %d KB\n", ESP.getFreePsram() / 1024);
    Serial.println("[HAL] --------------------------\n");

    p_engine->fillScreen(COLOR_BG_MAIN);
}

void widgets_swap() {
    p_engine->swapBuffers();
}

void drawWallpaperBackground() {
    if (WALLPAPER_BG_WIDTH == 480 && WALLPAPER_BG_HEIGHT == 320) {
        p_engine->drawRGB565Image(0, 0, WALLPAPER_BG_WIDTH, WALLPAPER_BG_HEIGHT,
                                  wallpaper_bg_rgb565);
    } else {
        p_engine->fillScreen(COLOR_BG_MAIN);
    }
}

// ── Widget Implementations ───────────────────────────────────────────────────

void drawCardBase(int x, int y, int w, int h, uint16_t color) {
    p_engine->fillRoundRect(x, y, w, h, CARD_RAD, color);
    p_engine->drawRoundRect(x, y, w, h, CARD_RAD, p_engine->color565(40, 50, 80));
}

void drawTempCard(int x, int y, int w, int h, const char* label, float temp, bool error) {
    drawCardBase(x, y, w, h, COLOR_CARD_BG);

    p_engine->setTextColor(COLOR_TEXT_SEC);
    p_engine->setTextFont(2);
    p_engine->drawString(label, x + 8, y + 8);

    if (error) {
        p_engine->setTextColor(COLOR_STAT_ERR);
        p_engine->drawString("ERROR", x + 8, y + 35);
    } else if (temp < -50.0f) {
        p_engine->setTextColor(COLOR_TEXT_SEC);
        p_engine->setTextFont(4);
        p_engine->drawString("NULL", x + 8, y + 32);
    } else {
        p_engine->setTextColor(COLOR_ACCENT_MAIN);
        p_engine->setTextFont(4);
        char val_buf[16];
        snprintf(val_buf, sizeof(val_buf), "%.1f", temp);
        p_engine->drawString(val_buf, x + 8, y + 32);

        int off_x = p_engine->textWidth(val_buf);
        p_engine->setTextFont(2);
        p_engine->drawString("C", x + 8 + off_x + 4, y + 35);
    }
}

void drawToggleButton(int x, int y, int w, int h, const char* label, bool state) {
    uint16_t btnColor = state ? COLOR_STAT_ON : COLOR_STAT_OFF;
    drawCardBase(x, y, w, h, COLOR_CARD_BG);

    int swW = 40, swH = 20;
    int swX = x + w - swW - 10;
    int swY = y + (h - swH) / 2;
    p_engine->fillRoundRect(swX, swY, swW, swH, 10, p_engine->color565(30, 40, 60));

    int kR = 8;
    int kX = state ? (swX + swW - kR - 2) : (swX + kR + 2);
    p_engine->fillCircle(kX, swY + swH / 2, kR, btnColor);

    p_engine->setTextColor(COLOR_TEXT_MAIN);
    p_engine->setTextFont(2);
    p_engine->setTextDatum(TextDatum::MiddleLeft);
    p_engine->drawString(label, x + 12, y + h / 2);
    p_engine->setTextDatum(TextDatum::TopLeft);
}

void drawUpDownButton(int x, int y, int w, int h, const char* label, bool isUp) {
    drawCardBase(x, y, w, h, COLOR_CARD_BG);

    p_engine->setTextFont(2);
    p_engine->setTextDatum(TextDatum::MiddleCenter);
    p_engine->drawString(label, x + w / 2, y + h / 2);
    p_engine->setTextDatum(TextDatum::TopLeft);
}

void drawPresenceBadge(int x, int y, int w, int h, bool detected) {
    drawCardBase(x, y, w, h, COLOR_CARD_BG);

    p_engine->setTextColor(COLOR_TEXT_SEC);
    p_engine->setTextFont(2);
    p_engine->drawString("Presence", x + 10, y + 8);

    uint16_t dotColor = detected ? COLOR_STAT_ON : COLOR_STAT_OFF;
    p_engine->fillCircle(x + 18, y + 35, 6, dotColor);

    p_engine->setTextColor(COLOR_TEXT_MAIN);
    p_engine->drawString(detected ? "Detected" : "Empty", x + 35, y + 27);
}

void drawCO2Card(int x, int y, int w, int h, int co2) {
    uint16_t valColor = COLOR_STAT_ON;
    if (co2 > 1500) valColor = COLOR_STAT_ERR;
    else if (co2 > 1000) valColor = COLOR_STAT_WARN;

    drawCardBase(x, y, w, h, COLOR_CARD_BG);

    p_engine->setTextFont(2);
    p_engine->setTextColor(COLOR_TEXT_SEC);
    p_engine->setTextDatum(TextDatum::TopLeft);
    p_engine->drawString("CO2", x + 10, y + 8);

    p_engine->setTextColor(valColor);
    p_engine->setTextFont(4);
    p_engine->setTextDatum(TextDatum::MiddleCenter);
    char buf[16];
    if (co2 == -1) {
        strcpy(buf, "NULL");
        p_engine->setTextColor(COLOR_TEXT_SEC);
    } else {
        snprintf(buf, 16, "%d", co2);
    }
    p_engine->drawString(buf, x + w / 2, y + h / 2 + 5);

    p_engine->setTextFont(2);
    p_engine->setTextColor(COLOR_TEXT_SEC);
    p_engine->setTextDatum(TextDatum::TopLeft);
    p_engine->drawString("ppm", x + w - 25, y + h - 15);
    p_engine->fillCircle(x + w - 15, y + 15, 4, valColor);
}

void drawNotifBar(bool wifi, bool lan, bool mqtt, const char* conn_status,
                  const char* room_name, const char* time_str) {
    p_engine->fillRect(0, 0, 480, 30, COLOR_BG_MAIN);
    p_engine->drawFastHLine(0, 29, 480, p_engine->color565(40, 50, 80));

    // ── Hamburger (x:8~38, bisa dicet sampai x:50) ──
    p_engine->fillRect(8, 8,  22, 3, COLOR_TEXT_MAIN);
    p_engine->fillRect(8, 14, 22, 3, COLOR_TEXT_MAIN);
    p_engine->fillRect(8, 20, 22, 3, COLOR_TEXT_MAIN);

    // ── Clock (center, zona aman 170-310) ──
    p_engine->setTextColor(COLOR_ACCENT_MAIN);
    p_engine->setTextFont(4);
    p_engine->setTextDatum(TextDatum::MiddleCenter);
    p_engine->drawString(time_str && strlen(time_str) > 0 ? time_str : "--:--", 240, 15);

    // ── Conn status (kiri tengah, x:50-160) ──
    if (conn_status && strlen(conn_status) > 0) {
        p_engine->setTextFont(1);
        if (strstr(conn_status, "Fail") || strstr(conn_status, "MISSING") || strstr(conn_status, "DISCONNECTED")) {
            p_engine->setTextColor(COLOR_STAT_ERR);
        } else if (strstr(conn_status, "Connecting") || strstr(conn_status, "RETRY")) {
            p_engine->setTextColor(p_engine->color565(255, 165, 0));
        } else {
            p_engine->setTextColor(COLOR_STAT_ON);
        }
        p_engine->setTextDatum(TextDatum::MiddleLeft);
        p_engine->drawString(conn_status, 48, 15);
    }

    // ── WiFi/LAN/MQTT dots (kanan, x:420-460) ──
    p_engine->fillCircle(424, 15, 4, wifi ? COLOR_STAT_ON : COLOR_STAT_ERR);
    p_engine->fillCircle(438, 15, 4, lan  ? COLOR_STAT_ON : COLOR_STAT_ERR);
    p_engine->fillCircle(452, 15, 4, mqtt ? COLOR_STAT_ON : COLOR_STAT_ERR);

    p_engine->setTextDatum(TextDatum::TopLeft);
}

void drawLuxCard(int x, int y, int w, int h, float lux) {
    drawCardBase(x, y, w, h, COLOR_CARD_BG);

    p_engine->setTextFont(2);
    p_engine->drawString("Luminance", x + 10, y + 8);

    char buf[16];
    if (lux < 0) {
        strcpy(buf, "NULL");
        p_engine->setTextColor(COLOR_TEXT_SEC);
    } else {
        snprintf(buf, sizeof(buf), "%.0f lx", lux);
        p_engine->setTextColor(COLOR_TEXT_MAIN);
    }

    p_engine->setTextFont(4);
    p_engine->setTextDatum(TextDatum::MiddleCenter);
    p_engine->drawString(buf, x + w / 2, y + h / 2 + 5);
    p_engine->setTextDatum(TextDatum::TopLeft);
}

void drawDashboardTopBar(const char* time_str, bool wifi, bool lan, bool bus_ok, bool slave_online, bool show_time) {
    drawSettingsLogoIcon(12, 8, 26);

    if (show_time) {
        p_engine->setTextDatum(TextDatum::MiddleLeft);
        p_engine->setTextFont(4);
        p_engine->setTextColor(COLOR_TEXT_MAIN);
        p_engine->drawString(time_str && strlen(time_str) > 0 ? time_str : "--:--", 46, 21);
    }

    const int chip_y = 10;
    const int chip_h = 22;
    const int chip_w = 48;
    const int gap = 8;
    const int start_x = 480 - 18 - (chip_w * 3) - (gap * 2);

    struct StatusChip {
        const char* label;
        bool active;
    } chips[3] = {
        {"WiFi", wifi},
        {"LAN", lan},
        {"BUS", bus_ok && slave_online}
    };

    p_engine->setTextDatum(TextDatum::MiddleCenter);
    p_engine->setTextFont(1);
    for (int i = 0; i < 3; i++) {
        int x = start_x + i * (chip_w + gap);
        uint16_t color = chips[i].active ? COLOR_STAT_ON : COLOR_STAT_OFF;
        p_engine->fillRoundRect(x, chip_y, chip_w, chip_h, 7, p_engine->color565(12, 18, 30));
        p_engine->drawRoundRect(x, chip_y, chip_w, chip_h, 7, color);
        p_engine->setTextColor(chips[i].active ? COLOR_TEXT_MAIN : COLOR_TEXT_SEC);
        p_engine->drawString(chips[i].label, x + chip_w / 2, chip_y + chip_h / 2);
    }
    p_engine->setTextDatum(TextDatum::TopLeft);
}

void drawCO2Chip(int x, int y, int co2) {
    char buf[24];
    snprintf(buf, sizeof(buf), "CO2 %d ppm", co2);
    p_engine->setTextFont(1);
    int w = p_engine->textWidth(buf) + 22;
    if (w < 96) w = 96;

    uint16_t color = COLOR_STAT_ON;
    if (co2 > 1500) color = COLOR_STAT_ERR;
    else if (co2 > 1000) color = COLOR_STAT_WARN;

    p_engine->fillRoundRect(x, y, w, 22, 7, p_engine->color565(12, 18, 30));
    p_engine->drawRoundRect(x, y, w, 22, 7, color);
    p_engine->setTextDatum(TextDatum::MiddleCenter);
    p_engine->setTextColor(COLOR_TEXT_MAIN);
    p_engine->drawString(buf, x + w / 2, y + 11);
    p_engine->setTextDatum(TextDatum::TopLeft);
}

void drawLargeTempWidget(int x, int y, int w, int h, float temp, bool valid, bool large) {
    drawCardBase(x, y, w, h, COLOR_CARD_BG);

    char value[20];
    if (valid) snprintf(value, sizeof(value), "%.1f C", temp);
    else snprintf(value, sizeof(value), "--");

    p_engine->setTextDatum(TextDatum::MiddleCenter);
    p_engine->setTextColor(valid ? COLOR_ACCENT_MAIN : COLOR_TEXT_SEC);
    p_engine->setTextFont(large ? 7 : 6);
    p_engine->drawString(value, x + w / 2, y + h / 2 - (large ? 14 : 8));

    p_engine->setTextFont(2);
    p_engine->setTextColor(COLOR_TEXT_SEC);
    p_engine->drawString("Average Room Temp", x + w / 2, y + h / 2 + (large ? 32 : 28));
    if (large) {
        p_engine->drawString("Tap for detail", x + w / 2, y + h - 24);
    }
    p_engine->setTextDatum(TextDatum::TopLeft);
}

void drawAcTargetWidget(int x, int y, int w, int h, float target_temp, bool ac_on) {
    drawCardBase(x, y, w, h, COLOR_CARD_BG);

    p_engine->setTextDatum(TextDatum::MiddleLeft);
    p_engine->setTextFont(2);
    p_engine->setTextColor(COLOR_TEXT_SEC);
    p_engine->drawString("AC Target", x + 14, y + 24);

    char target[16];
    snprintf(target, sizeof(target), "%.0f C", target_temp);
    p_engine->setTextFont(6);
    p_engine->setTextColor(COLOR_TEXT_MAIN);
    p_engine->drawString(target, x + 14, y + 54);

    uint16_t power_fill = ac_on ? COLOR_STAT_ON : COLOR_STAT_ERR;
    bool expanded_controls = h >= 180;
    int btn_h = expanded_controls ? 52 : 34;
    int btn_y = expanded_controls ? y + 90 : y + h - btn_h - 10;
    int chip_x = x + w - 88;
    int chip_y = y + 14;
    int chip_h = expanded_controls ? 64 : btn_y - chip_y - 4;
    if (chip_h < 42) chip_h = 42;
    int chip_w = 74;
    p_engine->fillRoundRect(chip_x, chip_y, chip_w, chip_h, 8, power_fill);
    p_engine->setTextDatum(TextDatum::MiddleCenter);
    p_engine->setTextFont(4);
    p_engine->setTextColor(COLOR_TEXT_MAIN);
    p_engine->drawString(ac_on ? "ON" : "OFF", chip_x + chip_w / 2, chip_y + chip_h / 2);

    int btn_w = (w - 30) / 2;
    p_engine->fillRoundRect(x + 10, btn_y, btn_w, btn_h, 8, COLOR_ACCENT_SEC);
    p_engine->fillRoundRect(x + 20 + btn_w, btn_y, btn_w, btn_h, 8, COLOR_STAT_OFF);
    p_engine->setTextFont(2);
    p_engine->setTextColor(COLOR_TEXT_MAIN);
    p_engine->drawString("UP", x + 10 + btn_w / 2, btn_y + btn_h / 2);
    p_engine->drawString("DOWN", x + 20 + btn_w + btn_w / 2, btn_y + btn_h / 2);
    p_engine->setTextDatum(TextDatum::TopLeft);
}

void drawLargeControlButton(int x, int y, int w, int h, const char* label, bool on, const char* subtext) {
    uint16_t border = on ? COLOR_STAT_ON : COLOR_STAT_OFF;
    uint16_t fill = on ? p_engine->color565(10, 48, 34) : COLOR_CARD_BG;

    if (subtext && strcmp(subtext, "POWERING") == 0) {
        border = p_engine->color565(255, 165, 0); // Orange
        fill = p_engine->color565(48, 34, 10);     // Orange-ish fill
    } else if (subtext && (strcmp(subtext, "FAIL") == 0 || strcmp(subtext, "CHK PROJ") == 0)) {
        border = COLOR_STAT_ERR; // Red
        fill = p_engine->color565(48, 10, 10);     // Red-ish fill
    } else if (subtext && strcmp(subtext, "CHK LUX") == 0) {
        border = COLOR_STAT_WARN;
        fill = p_engine->color565(48, 34, 10);
    }

    drawCardBase(x, y, w, h, fill);
    p_engine->drawRoundRect(x, y, w, h, CARD_RAD, border);

    p_engine->setTextDatum(TextDatum::MiddleCenter);
    p_engine->setTextFont(4);
    p_engine->setTextColor(COLOR_TEXT_MAIN);
    p_engine->drawString(label, x + w / 2, y + h / 2 - 12);
    p_engine->setTextFont(2);
    
    if (subtext) {
        if (strcmp(subtext, "POWERING") == 0) {
            p_engine->setTextColor(p_engine->color565(255, 165, 0));
            p_engine->drawString("POWERING", x + w / 2, y + h / 2 + 22);
        } else if (strcmp(subtext, "FAIL") == 0 || strcmp(subtext, "CHK PROJ") == 0) {
            p_engine->setTextColor(COLOR_STAT_ERR);
            p_engine->drawString("CHK PROJ", x + w / 2, y + h / 2 + 22);
        } else if (strcmp(subtext, "CHK LUX") == 0) {
            p_engine->setTextColor(COLOR_STAT_WARN);
            p_engine->drawString(subtext, x + w / 2, y + h / 2 + 22);
        } else if (strcmp(subtext, "NO LUX") == 0) {
            p_engine->setTextColor(COLOR_STAT_ERR);
            p_engine->drawString("NO LUX", x + w / 2, y + h / 2 + 22);
        } else {
            p_engine->setTextColor(on ? COLOR_STAT_ON : COLOR_TEXT_SEC);
            p_engine->drawString(subtext, x + w / 2, y + h / 2 + 22);
        }
    } else {
        p_engine->setTextColor(on ? COLOR_STAT_ON : COLOR_TEXT_SEC);
        p_engine->drawString(on ? "ON" : "OFF", x + w / 2, y + h / 2 + 22);
    }
    p_engine->setTextDatum(TextDatum::TopLeft);
}

void drawDashboardEmptyState(const char* title, const char* subtitle) {
    p_engine->setTextDatum(TextDatum::MiddleCenter);
    p_engine->setTextFont(4);
    p_engine->setTextColor(COLOR_TEXT_MAIN);
    p_engine->drawString(title, 240, 146);
    p_engine->setTextFont(2);
    p_engine->setTextColor(COLOR_TEXT_SEC);
    p_engine->drawString(subtitle, 240, 190);
    p_engine->setTextDatum(TextDatum::TopLeft);
}

void drawDashboardEmptyHero(const char* time_str, const char* title, const char* subtitle) {
    p_engine->setTextDatum(TextDatum::MiddleCenter);
    uint16_t primary_text = p_engine->color565(255, 255, 255);
    uint16_t helper_text = p_engine->color565(8, 8, 10);

    p_engine->setTextFont(7);
    p_engine->setTextColor(primary_text);
    p_engine->drawString(time_str && strlen(time_str) > 0 ? time_str : "--:--", 234, 146);

    p_engine->setTextFont(4);
    p_engine->setTextColor(primary_text);
    p_engine->drawString(title, 240, 204);

    p_engine->setTextFont(2);
    p_engine->setTextColor(helper_text);
    p_engine->drawString(subtitle, 240, 236);

    p_engine->setTextDatum(TextDatum::TopLeft);
}
