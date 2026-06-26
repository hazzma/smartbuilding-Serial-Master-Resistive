#include "lan_manager.h"
#include "display.h"   // LAN pin defines (LAN_SCK, LAN_MISO, etc.)
#include <SPI.h>
#include <Ethernet.h>
#include <Preferences.h>
#include <Dns.h>
#include "data.h"

SPIClass lanSPI(2); // SPI2_HOST (HSPI) — dedicated to W5500, no conflict with TFT SPI3
static bool lan_needs_restart = false;
static bool lan_initialized_once = false;

static void lan_format_ip_or_dash(const IPAddress& ip, char* out, size_t out_size) {
    if (out_size == 0) return;
    if (ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0) {
        strncpy(out, "-", out_size - 1);
    } else {
        snprintf(out, out_size, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    }
    out[out_size - 1] = '\0';
}

static void lan_update_runtime_state(const char* detail_override = nullptr) {
    EthernetLinkStatus link_status = Ethernet.linkStatus();
    IPAddress ip = Ethernet.localIP();
    IPAddress gw = Ethernet.gatewayIP();
    IPAddress sn = Ethernet.subnetMask();
    IPAddress dns = Ethernet.dnsServerIP();

    // Koneksi dianggap "connected" hanya jika kabel ON DAN IP valid (bukan 0.0.0.0)
    bool ip_valid = (ip[0] != 0 || ip[1] != 0 || ip[2] != 0 || ip[3] != 0);
    bool actually_connected = (link_status == LinkON) && ip_valid;

    char ip_buf[16];
    char gw_buf[16];
    char sn_buf[16];
    char dns_buf[16];
    lan_format_ip_or_dash(ip, ip_buf, sizeof(ip_buf));
    lan_format_ip_or_dash(gw, gw_buf, sizeof(gw_buf));
    lan_format_ip_or_dash(sn, sn_buf, sizeof(sn_buf));
    lan_format_ip_or_dash(dns, dns_buf, sizeof(dns_buf));

    data_lock(g_state);
    g_state.net.lan_connected = actually_connected;
    strncpy(g_state.net.lan_ip, ip_buf, sizeof(g_state.net.lan_ip) - 1);
    strncpy(g_state.net.lan_current_gateway, gw_buf, sizeof(g_state.net.lan_current_gateway) - 1);
    strncpy(g_state.net.lan_current_subnet, sn_buf, sizeof(g_state.net.lan_current_subnet) - 1);
    strncpy(g_state.net.lan_current_dns, dns_buf, sizeof(g_state.net.lan_current_dns) - 1);
    g_state.net.lan_ip[sizeof(g_state.net.lan_ip) - 1] = '\0';
    g_state.net.lan_current_gateway[sizeof(g_state.net.lan_current_gateway) - 1] = '\0';
    g_state.net.lan_current_subnet[sizeof(g_state.net.lan_current_subnet) - 1] = '\0';
    g_state.net.lan_current_dns[sizeof(g_state.net.lan_current_dns) - 1] = '\0';

    const char* link_label = "Unknown";
    if (link_status == LinkON && ip_valid)  link_label = "Cable connected";
    else if (link_status == LinkON)         link_label = "Cable ON (no IP)";
    else if (link_status == LinkOFF)        link_label = "Cable unplugged";
    strncpy(g_state.net.lan_link_status, link_label, sizeof(g_state.net.lan_link_status) - 1);
    g_state.net.lan_link_status[sizeof(g_state.net.lan_link_status) - 1] = '\0';

    if (detail_override) {
        strncpy(g_state.net.lan_status_detail, detail_override, sizeof(g_state.net.lan_status_detail) - 1);
        g_state.net.lan_status_detail[sizeof(g_state.net.lan_status_detail) - 1] = '\0';
    }
    g_state.ui_needs_update = true;
    data_unlock(g_state);
}

void lan_manager_load_config() {
    Preferences prefs;
    prefs.begin("lan_config", true);
    data_lock(g_state);
    g_state.net.lan_use_dhcp = prefs.getBool("dhcp", true);
    String ip = prefs.getString("static_ip", "192.168.1.177");
    String gw = prefs.getString("gateway",   "192.168.1.1");
    String sn = prefs.getString("subnet",    "255.255.255.0");
    String dns = prefs.getString("dns",      "8.8.8.8");
    strncpy(g_state.net.lan_static_ip, ip.c_str(), 16);
    strncpy(g_state.net.lan_gateway,   gw.c_str(), 16);
    strncpy(g_state.net.lan_subnet,    sn.c_str(), 16);
    strncpy(g_state.net.lan_dns,       dns.c_str(), 16);
    data_unlock(g_state);
    prefs.end();
}

void lan_manager_save_config() {
    Preferences prefs;
    prefs.begin("lan_config", false);
    data_lock(g_state);
    prefs.putBool("dhcp",      g_state.net.lan_use_dhcp);
    prefs.putString("static_ip", g_state.net.lan_static_ip);
    prefs.putString("gateway",   g_state.net.lan_gateway);
    prefs.putString("subnet",    g_state.net.lan_subnet);
    prefs.putString("dns",       g_state.net.lan_dns);
    data_unlock(g_state);
    prefs.end();
    Serial.println("[LAN] Config Saved. Scheduling async restart...");
    lan_needs_restart = true;
}

void lan_check_internet() {
    if (Ethernet.linkStatus() != LinkON) {
        lan_update_runtime_state("Cable unplugged");
        return;
    }

    data_lock(g_state);
    g_state.net.lan_checking = true;
    g_state.ui_needs_update = true;
    data_unlock(g_state);

    DNSClient dns;
    dns.begin(Ethernet.dnsServerIP());
    IPAddress remote_ip;
    bool has_internet = (dns.getHostByName("google.com", remote_ip) == 1);
    lan_update_runtime_state(has_internet ? "Internet Access OK" : "Local Only (No Internet)");

    data_lock(g_state);
    g_state.net.lan_checking = false;
    data_unlock(g_state);
}

void lan_init() {
    Serial.println("[LAN] Initializing Ethernet (W5500 / SPI2)...");

    data_lock(g_state);
    g_state.net.lan_initialized = false;
    g_state.net.lan_dhcp_ok = false;
    g_state.net.lan_static_fallback = false;
    g_state.net.lan_checking = false;
    strncpy(g_state.net.lan_status_detail, "Initializing Ethernet", sizeof(g_state.net.lan_status_detail) - 1);
    g_state.net.lan_status_detail[sizeof(g_state.net.lan_status_detail) - 1] = '\0';
    g_state.ui_needs_update = true;
    data_unlock(g_state);

    lan_manager_load_config();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    mac[5] ^= 0x01;

    pinMode(LAN_RST, OUTPUT);
    digitalWrite(LAN_RST, LOW);
    vTaskDelay(pdMS_TO_TICKS(100));
    digitalWrite(LAN_RST, HIGH);
    vTaskDelay(pdMS_TO_TICKS(200));

    lanSPI.begin(LAN_SCK, LAN_MISO, LAN_MOSI, LAN_CS);
    Ethernet.init(LAN_CS);

    bool success = false;
    data_lock(g_state);
    bool use_dhcp = g_state.net.lan_use_dhcp;
    data_unlock(g_state);

    if (use_dhcp) {
        Serial.println("[LAN] Trying DHCP...");
        data_lock(g_state);
        strncpy(g_state.net.lan_status_detail, "Requesting DHCP lease", sizeof(g_state.net.lan_status_detail) - 1);
        g_state.net.lan_status_detail[sizeof(g_state.net.lan_status_detail) - 1] = '\0';
        g_state.ui_needs_update = true;
        data_unlock(g_state);

        if (Ethernet.begin(mac, 1500, 500) != 0) {
            success = true;
        } else {
            Serial.println("[LAN] DHCP Failed.");
            lan_update_runtime_state("DHCP failed, using static fallback");
        }
    }

    if (!success) {
        Serial.println("[LAN] Using Static IP...");
        IPAddress ip, gw, sn, dns;
        data_lock(g_state);
        ip.fromString(g_state.net.lan_static_ip);
        gw.fromString(g_state.net.lan_gateway);
        sn.fromString(g_state.net.lan_subnet);
        dns.fromString(g_state.net.lan_dns);
        data_unlock(g_state);
        Ethernet.begin(mac, ip, dns, gw, sn);
    }

    lan_update_runtime_state(success ? "DHCP lease acquired" : "Static IP configured");

    data_lock(g_state);
    g_state.net.lan_initialized = true;
    g_state.net.lan_dhcp_ok = success;
    g_state.net.lan_static_fallback = !success && use_dhcp;
    data_unlock(g_state);

    Serial.print("[LAN] Ready. IP: ");
    Serial.println(Ethernet.localIP());
    lan_check_internet();
    lan_initialized_once = true;
}

void lan_loop() {
    if (lan_needs_restart) {
        lan_needs_restart = false;
        lan_init();
        return;
    }

    data_lock(g_state);
    bool initialized = g_state.net.lan_initialized;
    data_unlock(g_state);
    if (!initialized) return;

    static uint32_t last_link_check    = 0;
    static uint32_t last_internet_check = 0;

    if (millis() - last_link_check > 2000) {
        last_link_check = millis();
        bool link = (Ethernet.linkStatus() == LinkON);
        IPAddress ip = Ethernet.localIP();
        bool ip_valid = (ip[0] != 0 || ip[1] != 0 || ip[2] != 0 || ip[3] != 0);
        bool actually_connected = link && ip_valid;

        data_lock(g_state);
        bool was_connected = g_state.net.lan_connected;
        data_unlock(g_state);

        if (was_connected != actually_connected || (!ip_valid && link)) {
            Serial.printf("[LAN] Status Changed: link=%s ip_valid=%s connected=%s\n",
                          link ? "ON" : "OFF",
                          ip_valid ? "yes" : "no",
                          actually_connected ? "CONNECTED" : "DISCONNECTED");

            if (link && !ip_valid) {
                // Kabel ON tapi IP 0.0.0.0 — static IP belum terpasang, trigger reinit
                Serial.println("[LAN] Link ON but no IP — reinitializing Ethernet");
                lan_update_runtime_state("Link ON: re-applying IP config");
                lan_needs_restart = true;
            } else {
                lan_update_runtime_state(actually_connected ? nullptr : "Cable unplugged");
            }
        }
    }

    if (millis() - last_internet_check > 30000) {
        last_internet_check = millis();
        lan_check_internet();
    }
}

bool is_lan_connected() {
    return Ethernet.linkStatus() == LinkON;
}

void Task_LAN(void* pvParameters) {
    Serial.println("[LAN] Task started on Core 0");
    vTaskDelay(pdMS_TO_TICKS(500));
    lan_init();
    uint32_t last_diag = 0;
    for (;;) {
        lan_loop();
        if (millis() - last_diag > 10000) {
            last_diag = millis();
            Serial.printf("[LAN] Diag | Heap:%u | Stack:%u | Init:%u\n",
                          (unsigned)ESP.getFreeHeap(),
                          (unsigned)uxTaskGetStackHighWaterMark(NULL),
                          lan_initialized_once);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void lan_task_init() {
    xTaskCreatePinnedToCore(Task_LAN, "LAN_Task", 4096, NULL, 1, NULL, 0);
}
