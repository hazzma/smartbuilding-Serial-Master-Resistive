#ifndef DISPLAY_H
#define DISPLAY_H

// ─────────────────────────────────────────────────────────────────────────────
// Pin Definitions - ILI9488 SPI via LovyanGFX
// ─────────────────────────────────────────────────────────────────────────────
#define TFT_CS    17
#define TFT_RST   16
#define TFT_DC    15
#define TFT_MOSI  7
#define TFT_SCLK  6
#define TFT_BL    5
#define TFT_MISO  4

// Touch XPT2046 — shares TFT SPI3 data/clock lines, dedicated chip select.
#define TOUCH_CS   46
#define TOUCH_IRQ  -1   // Not connected; touch is polled.

// Ethernet W5500 — SPI2_HOST
#define LAN_SCK   12
#define LAN_MISO  13
#define LAN_MOSI  11
#define LAN_CS    10
#define LAN_RST   18

// ─────────────────────────────────────────────────────────────────────────────
// LovyanGFX Configuration — ILI9488 480x320 Serial SPI
// ─────────────────────────────────────────────────────────────────────────────
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ILI9488 _panel_instance;
    lgfx::Bus_SPI       _bus_instance;
    lgfx::Light_PWM     _light_instance;
    lgfx::Touch_XPT2046 _touch_instance;

public:
    LGFX() {
        // SPI Bus Config
        {
            auto cfg = _bus_instance.config();
            cfg.spi_host   = SPI3_HOST;  // VSPI
            cfg.spi_mode   = 0;
            cfg.freq_write = 60000000;   // 60 MHz write: faster than 40 MHz, more stable than 80 MHz on this PCB
            cfg.freq_read  = 20000000;
            cfg.spi_3wire  = false;      // 4-wire SPI (with MISO)
            cfg.use_lock   = true;
            cfg.dma_channel= SPI_DMA_CH_AUTO;
            cfg.pin_sclk   = TFT_SCLK;
            cfg.pin_mosi   = TFT_MOSI;
            cfg.pin_miso   = TFT_MISO;
            cfg.pin_dc     = TFT_DC;
            _bus_instance.config(cfg);
            _panel_instance.setBus(&_bus_instance);
        }

        // Panel Config
        {
            auto cfg = _panel_instance.config();
            cfg.pin_cs           = TFT_CS;
            cfg.pin_rst          = TFT_RST;
            cfg.pin_busy         = -1;
            cfg.panel_width      = 320;
            cfg.panel_height     = 480;
            cfg.offset_x         = 0;
            cfg.offset_y         = 0;
            cfg.offset_rotation  = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable         = true;
            cfg.invert           = false;
            cfg.rgb_order        = false;
            cfg.dlen_16bit       = false;
            cfg.bus_shared       = true;  // Shared only with XPT2046; W5500 stays on SPI2
            _panel_instance.config(cfg);
        }

        // Resistive touch controller shares the TFT SPI3 bus.
        {
            auto cfg = _touch_instance.config();
            cfg.spi_host        = SPI3_HOST;
            cfg.pin_sclk        = TFT_SCLK;
            cfg.pin_mosi        = TFT_MOSI;
            cfg.pin_miso        = TFT_MISO;
            cfg.pin_cs          = TOUCH_CS;
            cfg.pin_int         = TOUCH_IRQ;
            cfg.freq            = 1000000;
            cfg.x_min           = 3900;
            cfg.x_max           = 300;
            cfg.y_min           = 400;
            cfg.y_max           = 3900;
            cfg.bus_shared      = true;
            cfg.offset_rotation = 0;
            _touch_instance.config(cfg);
            _panel_instance.setTouch(&_touch_instance);
        }

        // Backlight Config (PWM)
        {
            auto cfg = _light_instance.config();
            cfg.pin_bl     = TFT_BL;
            cfg.invert     = false;
            cfg.freq       = 44100;
            cfg.pwm_channel = 7;
            _light_instance.config(cfg);
            _panel_instance.setLight(&_light_instance);
        }

        setPanel(&_panel_instance);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// UI Design Language (FSD 5.1) — Color Tokens
// ─────────────────────────────────────────────────────────────────────────────
#define COLOR_BG_MAIN       0x0002      // #000010 (Hampir hitam, kontras tinggi)
#define COLOR_ACCENT_MAIN   0x07FF      // #00FFFF (Cyan murni)
#define COLOR_ACCENT_SEC    0x03FF      // #007FFF (Azure vibrant)
#define COLOR_CARD_BG       0x0863      // #0A0E1A (Dark navy)
#define COLOR_TEXT_MAIN     0xFFFF      // Putih bersih
#define COLOR_TEXT_SEC      0xAD55      // Abu-abu kebiruan terang
#define COLOR_STAT_ON       0x07E0      // Hijau murni
#define COLOR_STAT_OFF      0x39E7      // Abu-abu gelap
#define COLOR_STAT_ERR      0xF800      // Merah murni
#define COLOR_STAT_WARN     0xFD20      // Orange vibrant

// ─────────────────────────────────────────────────────────────────────────────
// Globals
// ─────────────────────────────────────────────────────────────────────────────
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

extern LGFX tft;
extern SemaphoreHandle_t bus_mutex;

void display_init();
void display_brightness(uint8_t pct); // 0-100

#endif
