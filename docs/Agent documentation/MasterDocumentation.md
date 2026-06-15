# Master Documentation - Smart Building Master S3 (Serial Edition)

## 1. Project Overview & Understanding

Proyek ini memakai **Serial SPI** untuk display dan **XPT2046 Resistive Touch SPI** untuk input, serta mengadopsi arsitektur **Agent-Based** untuk skalabilitas kode.

### Key Changes from Previous Version (FSD 1.1):
- **Display**: Migrasi dari ILI9488 Parallel 8-bit ke **ILI9488 SPI (Serial)**.
- **Touch**: XPT2046 resistive touch berbagi SCLK/MOSI/MISO dengan TFT dan memakai CS terpisah.
- **Graphics Library**: Menggunakan **LovyanGFX** sebagai engine utama (menggantikan TFT_eSPI atau driver manual).
- **UI Framework**: Mengintegrasikan library custom [hazzma/SmartBuildingUI](https://github.com/hazzma/UI-SmartBuilding).
- **Communication**: Penambahan modul **RS485** untuk komunikasi ke Slave/perangkat industri.
- **Architecture**: Transisi ke **General Master Agent** dengan sub-agent spesialis.

---

## 2. Proposed Hardware Pinout (ESP32-S3)

Dengan berpindah ke SPI, kita membebaskan banyak pin GPIO yang sebelumnya digunakan untuk bus data 8-bit.

### 2.1 Display & Touch (LovyanGFX SPI Bus)
| Function | Pin (GPIO) | Notes |
| :--- | :--- | :--- |
| **TFT_CS** | 17 | Chip Select |
| **TFT_RST** | 16 | Reset |
| **TFT_DC** | 15 | Data/Command |
| **TFT_MOSI** | 7 | SPI3 MOSI |
| **TFT_SCLK** | 6 | SPI3 SCK |
| **TFT_BL** | 5 | Backlight PWM |
| **TFT_MISO** | 4 | SPI3 MISO (Optional for TFT) |
| **TP_CS** | 46 | XPT2046 chip select |
| **TP_CLK** | 6 | Shared SPI3 SCLK with TFT |
| **TP_DIN** | 7 | Shared SPI3 MOSI with TFT |
| **TP_DO** | 4 | Shared SPI3 MISO with TFT |
| **TP_IRQ** | - | Not connected / polling used |

### 2.2 Ethernet (W5500 SPI Bus)
| Function | Pin (GPIO) | Notes |
| :--- | :--- | :--- |
| **ETH_SCLK** | 12 | SPI2_HOST SCK |
| **ETH_MOSI** | 11 | SPI2_HOST MOSI |
| **ETH_MISO** | 13 | SPI2_HOST MISO |
| **ETH_CS** | 10 | Chip Select |
| **ETH_RST** | 18 | Reset |
| **ETH_INT** | - | Not connected / unused |

### 2.3 RS485 Communication (UART0 / board TXD0-RXD0)
| Function | Pin (GPIO) | Notes |
| :--- | :--- | :--- |
| **RS485_TX** | `TX` / TXD0 | UART0 TX to MAX3485 DI |
| **RS485_RX** | `RX` / RXD0 | UART0 RX from MAX3485 RO |
| **RS485_DE** | 21 / COM_SW | MAX3485 DE + /RE direction |

USB CDC remains the debug console, so hardware UART0 can be used for the RS485 field bus.

---

## 3. ASTER Agent-Based Architecture

Sistem dikelola oleh **ASTER** (General Master Agent) yang berada di direktori khusus `/agents`. Arsitektur ini memisahkan definisi kapabilitas (Skills) dari implementasi teknis.

### Sub-Agent Structure:
Setiap agent memiliki folder tersendiri yang berisi definisi perannya:
- **[UI Agent](./agents/UIAgent/README.md)**: Logic navigasi & SmartBuildingUI.
- **[Display Agent](./agents/DisplayAgent/README.md)**: Driver LovyanGFX & Touch.
- **[WiFi Agent](./agents/WiFiAgent/README.md)**: Manajemen konektivitas wireless.
- **[LAN Agent](./agents/LANAgent/README.md)**: Manajemen Ethernet W5500.
- **[Data Agent](./agents/DataAgent/README.md)**: State management terpusat.
- **[Comm Agent](./agents/CommAgent/README.md)**: Protokol RS485.

---

## 4. Software Design Philosophy (The Master Agent)

1. **Decoupling**: Setiap agent berjalan dalam task RTOS sendiri (jika diperlukan) atau di-poll oleh Master Agent secara non-blocking.
2. **Event-Driven**: Perubahan state pada satu agent (misal: RS485 menerima data baru) akan memicu update di Data Agent, yang kemudian di-notif ke UI Agent untuk re-render.
3. **Safety**: Akses hardware tetap diproteksi `bus_mutex`, terutama jika SPI atau I2C digunakan bersama.
4. **Resilience**: Jika WiFi gagal, LAN harus bisa membackup, dan sebaliknya, dengan RS485 tetap berjalan independen.

---

## 5. Development Roadmap (Phase 1)
1. Setup `platformio.ini` dengan environment S3 dan library `LovyanGFX` + `SmartBuildingUI`.
2. Implementasi `DisplayAgent` (LovyanGFX Setup).
3. Implementasi `DataAgent` (Global State & Mutex).
4. Integrasi `SmartBuildingUI` ke `UIAgent`.
5. Implementasi Network & Comm Agents.
