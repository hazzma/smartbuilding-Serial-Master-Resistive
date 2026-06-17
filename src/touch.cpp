#include "touch.h"
#include "display.h"
#include "data.h"
#include <lgfx/v1/platforms/common.hpp>

static bool touch_cs_output_ok = false;
static bool     touch_was_pressed = false;
static uint32_t last_valid_press  = 0;
static int      last_touch_x = 0;
static int      last_touch_y = 0;
static uint32_t last_move_ts = 0;
static uint32_t last_diag_ts = 0;
static uint32_t last_raw_log_ts = 0;

static int32_t touch_correct_screen_x(int32_t x) {
    if (x <= 29) {
        return (x * 35) / 29;
    }
    if (x <= 251) {
        return 35 + ((x - 29) * 205) / 222;
    }
    return 240 + ((x - 251) * 205) / 229;
}

static int32_t touch_correct_screen_y(int32_t y) {
    if (y <= 70) {
        return y;
    }
    if (y <= 182) {
        return 70 + ((y - 70) * 110) / 112;
    }
    return 180 + ((y - 182) * 110) / 118;
}

static uint16_t touch_spi_read_adc(uint8_t command) {
    uint8_t data[3] = {command, 0x00, 0x00};
    lgfx::spi::readBytes(SPI3_HOST, data, sizeof(data));
    uint16_t value = (uint16_t)data[1] << 8;
    value |= data[2];
    return (value >> 3) & 0x0FFF;
}

static void touch_probe_direct_spi() {
    if (!touch_cs_output_ok) return;
    if (bus_mutex && xSemaphoreTake(bus_mutex, pdMS_TO_TICKS(30)) != pdTRUE) {
        Serial.println("[TC][PROBE] Cannot acquire shared SPI bus");
        return;
    }

    pinMode(TFT_CS, OUTPUT);
    pinMode(TOUCH_CS, OUTPUT);

    digitalWrite(TFT_CS, HIGH);
    lgfx::spi::beginTransaction(SPI3_HOST, 1000000, 0);
    digitalWrite(TOUCH_CS, LOW);
    delayMicroseconds(5);

    int miso_idle = digitalRead(TFT_MISO);
    uint16_t x = touch_spi_read_adc(0xD0);
    uint16_t y = touch_spi_read_adc(0x90);
    uint16_t z1 = touch_spi_read_adc(0xB0);
    uint16_t z2 = touch_spi_read_adc(0xC0);

    digitalWrite(TOUCH_CS, HIGH);
    lgfx::spi::endTransaction(SPI3_HOST);
    if (bus_mutex) xSemaphoreGive(bus_mutex);

    Serial.printf("[TC][PROBE] direct SPI MISO:%d X:%u Y:%u Z1:%u Z2:%u\n",
                  miso_idle, x, y, z1, z2);
    if ((x == 0 && y == 0 && z1 == 0 && z2 == 0) ||
        (x == 4095 && y == 4095 && z1 == 4095 && z2 == 4095)) {
        Serial.println("[TC][PROBE][ERROR] MISO response is stuck; check T_DO wiring, T_CS wiring, power, and controller type.");
    }
}

static void touch_test_cs_pin() {
    pinMode(TOUCH_CS, OUTPUT);

    digitalWrite(TOUCH_CS, HIGH);
    delayMicroseconds(10);
    int high_read = digitalRead(TOUCH_CS);

    digitalWrite(TOUCH_CS, LOW);
    delayMicroseconds(10);
    int low_read = digitalRead(TOUCH_CS);

    digitalWrite(TOUCH_CS, HIGH);
    touch_cs_output_ok = high_read == HIGH && low_read == LOW;

    Serial.printf("[TC][DIAG] CS GPIO%d output test: HIGH read=%d, LOW read=%d => %s\n",
                  TOUCH_CS, high_read, low_read, touch_cs_output_ok ? "PASS" : "FAIL");
    if (!touch_cs_output_ok) {
        Serial.println("[TC][ERROR] XPT2046 CS cannot be driven HIGH/LOW; check pin capability and wiring.");
    }
}

void touch_init() {
    Serial.println("[TC] --- Touch Initialization ---");
    Serial.printf("[TC] XPT2046 resistive SPI | CS:%d SCLK:%d MOSI:%d MISO:%d IRQ:unused\n",
                  TOUCH_CS, TFT_SCLK, TFT_MOSI, TFT_MISO);
    Serial.println("[TC] Display driver: LovyanGFX Panel_ILI9488 over SPI3");
    touch_test_cs_pin();
    Serial.println("[TC][DIAG] Touch raw/coordinate diagnostics enabled.");
    touch_probe_direct_spi();
}

static bool touch_read_current(int &tx, int &ty, bool &pressed) {
    pressed = false;

    lgfx::touch_point_t raw;
    uint_fast8_t raw_count = 0;
    int32_t x = -1;
    int32_t y = -1;
    if (bus_mutex && xSemaphoreTake(bus_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        raw_count = tft.getTouchRaw(&raw);
        pressed = tft.getTouch(&x, &y);
        xSemaphoreGive(bus_mutex);
    } else if (!bus_mutex) {
        raw_count = tft.getTouchRaw(&raw);
        pressed = tft.getTouch(&x, &y);
    } else {
        uint32_t now = millis();
        if (now - last_diag_ts >= 2000) {
            last_diag_ts = now;
            Serial.println("[TC][WARN] SPI bus mutex timeout while reading touch");
        }
        return false;
    }

    // The installed resistive overlay is mounted 180 degrees from the display.
    // Keep LovyanGFX touch reading untouched and invert only final screen coordinates.
    if (pressed) {
        x = (tft.width() - 1) - x;
        y = (tft.height() - 1) - y;
        x = touch_correct_screen_x(x);
        y = touch_correct_screen_y(y);
        if (x < 0 || x >= tft.width() || y < 0 || y >= tft.height()) {
            Serial.printf("[TC][WARN] Rejected out-of-screen mapped coordinate: %ld,%ld\n",
                          (long)x, (long)y);
            pressed = false;
        }
    }

    uint32_t now = millis();
    if (raw_count && now - last_raw_log_ts >= 100) {
        last_raw_log_ts = now;
        Serial.printf("[TC][RAW] x:%d y:%d pressure:%u | mapped:%ld,%ld pressed:%s\n",
                      raw.x, raw.y, raw.size, (long)x, (long)y, pressed ? "YES" : "NO");
    } else if (!raw_count && now - last_diag_ts >= 2000) {
        last_diag_ts = now;
        Serial.printf("[TC][DIAG] No valid XPT2046 touch sample | CS output:%s GPIO%d level:%d\n",
                      touch_cs_output_ok ? "PASS" : "FAIL", TOUCH_CS, digitalRead(TOUCH_CS));
        touch_probe_direct_spi();
    }

    static bool last_pressed_state = false;
    bool state_changed = (pressed != last_pressed_state);
    last_pressed_state = pressed;

    data_lock(g_state);
    g_state.touch_pressed = pressed;
    if (pressed) {
        g_state.touch_x = (int)x;
        g_state.touch_y = (int)y;
        g_state.touch_raw_x = raw.x;
        g_state.touch_raw_y = raw.y;
        g_state.touch_last_x = (int)x;
        g_state.touch_last_y = (int)y;
        g_state.touch_last_raw_x = raw.x;
        g_state.touch_last_raw_y = raw.y;
        g_state.ui_needs_update = true;
    } else {
        g_state.touch_x = -1;
        g_state.touch_y = -1;
        g_state.touch_raw_x = 0;
        g_state.touch_raw_y = 0;
        if (state_changed) {
            g_state.ui_needs_update = true;
        }
    }
    data_unlock(g_state);

    if (pressed) {
        tx = (int)x;
        ty = (int)y;
    }
    return true;
}

bool touch_get_event(int &tx, int &ty, TouchEventType &event) {
    event = TOUCH_EVENT_NONE;

    bool pressed = false;
    if (!touch_read_current(tx, ty, pressed)) return false;

    uint32_t now = millis();

    if (!pressed) {
        if (touch_was_pressed) {
            touch_was_pressed = false;
            tx = last_touch_x;
            ty = last_touch_y;
            event = TOUCH_EVENT_UP;
            return true;
        }
        return false;
    }

    if (!touch_was_pressed && (now - last_valid_press > 80)) {
        touch_was_pressed = true;
        last_valid_press = now;
        last_move_ts = now;
        last_touch_x = tx;
        last_touch_y = ty;
        event = TOUCH_EVENT_DOWN;
        Serial.printf("[TC] Touch down X:%d Y:%d\n", tx, ty);
        return true;
    }

    if (!touch_was_pressed) return false;

    int dx = abs(tx - last_touch_x);
    int dy = abs(ty - last_touch_y);
    if ((dx >= 2 || dy >= 2) && (now - last_move_ts >= 20)) {
        last_touch_x = tx;
        last_touch_y = ty;
        last_move_ts = now;
        event = TOUCH_EVENT_MOVE;
        return true;
    }

    return false;
}

bool touch_get_point(int &tx, int &ty) {
    TouchEventType event = TOUCH_EVENT_NONE;
    if (!touch_get_event(tx, ty, event)) return false;

    return event == TOUCH_EVENT_DOWN;
}
