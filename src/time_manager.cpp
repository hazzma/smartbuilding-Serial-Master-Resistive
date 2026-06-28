#include "time_manager.h"
#include "data.h"

#define Serial Serial
#include <Dns.h>
#include <sys/time.h>

static EthernetUDP udp;
static const char* ntpServer = "pool.ntp.org";
static const int NTP_PACKET_SIZE = 48;
static byte packetBuffer[NTP_PACKET_SIZE];

enum class LanNtpState : uint8_t {
    IDLE,
    WAIT_RESPONSE
};

static LanNtpState lan_ntp_state = LanNtpState::IDLE;
static IPAddress lan_ntp_ip;
static uint32_t lan_ntp_sent_ms = 0;
static uint32_t last_lan_sync_attempt_ms = 0;
static uint32_t last_success_ms = 0;
static uint8_t lan_ntp_fail_count = 0;
static bool wifi_ntp_started = false;
static bool rtc_time_valid = false;

static const uint32_t LAN_NTP_RESPONSE_TIMEOUT_MS = 3000;
static const uint32_t LAN_NTP_RETRY_MS = 15000;
static const uint32_t NTP_RESYNC_MS = 600000;

static void time_set_status(const char* status, const char* source, bool syncing, bool synced) {
    data_lock(g_state);
    g_state.net.time_syncing = syncing;
    g_state.net.time_synced = synced;
    strncpy(g_state.net.time_status, status, sizeof(g_state.net.time_status) - 1);
    strncpy(g_state.net.time_source, source, sizeof(g_state.net.time_source) - 1);
    g_state.net.time_status[sizeof(g_state.net.time_status) - 1] = '\0';
    g_state.net.time_source[sizeof(g_state.net.time_source) - 1] = '\0';
    g_state.ui_needs_update = true;
    data_unlock(g_state);
}

static bool time_update_display_string(const char* source) {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo, 5)) return false;
    if (timeinfo.tm_year < (2024 - 1900)) return false;

    char next_time[16];
    strftime(next_time, sizeof(next_time), "%H:%M", &timeinfo);

    const bool rtc_source = strcmp(source, "RTC") == 0;
    const bool manual_source = strcmp(source, "Manual") == 0;
    data_lock(g_state);
    bool changed = strcmp(g_state.net.time_str, next_time) != 0 ||
                   !g_state.net.time_synced ||
                   strcmp(g_state.net.time_source, source) != 0;
    strncpy(g_state.net.time_str, next_time, sizeof(g_state.net.time_str) - 1);
    g_state.net.time_str[sizeof(g_state.net.time_str) - 1] = '\0';
    g_state.net.time_synced = true;
    g_state.net.time_syncing = false;
    strncpy(g_state.net.time_source, source, sizeof(g_state.net.time_source) - 1);
    g_state.net.time_source[sizeof(g_state.net.time_source) - 1] = '\0';
    strncpy(g_state.net.time_status,
            rtc_source ? "RTC running" : (manual_source ? "Manual time" : "Time synced"),
            sizeof(g_state.net.time_status) - 1);
    g_state.net.time_status[sizeof(g_state.net.time_status) - 1] = '\0';
    if (changed) g_state.ui_needs_update = true;
    data_unlock(g_state);
    rtc_time_valid = true;
    return true;
}

void time_manager_init() {
    configTime(7 * 3600, 0, ntpServer);
    wifi_ntp_started = false;
    rtc_time_valid = false;
    time_set_status("Waiting for NTP", "-", false, false);
}

static void sendNTPpacket(const IPAddress& address) {
    memset(packetBuffer, 0, NTP_PACKET_SIZE);
    packetBuffer[0] = 0b11100011;
    packetBuffer[1] = 0;
    packetBuffer[2] = 6;
    packetBuffer[3] = 0xEC;
    packetBuffer[12] = 49;
    packetBuffer[13] = 0x4E;
    packetBuffer[14] = 49;
    packetBuffer[15] = 52;
    udp.beginPacket(address, 123);
    udp.write(packetBuffer, NTP_PACKET_SIZE);
    udp.endPacket();
}

static bool lan_ntp_resolve(IPAddress& ntp_ip) {
    DNSClient dns;
    dns.begin(Ethernet.dnsServerIP());
    
    const char* ntpServers[] = {
        "pool.ntp.org",
        "time.google.com",
        "time.windows.com"
    };

    for (int i = 0; i < 3; i++) {
        if (dns.getHostByName(ntpServers[i], ntp_ip) == 1) {
            return true;
        }
    }

    // Try directly with 8.8.8.8 dns if local dns fails
    dns.begin(IPAddress(8, 8, 8, 8));
    for (int i = 0; i < 3; i++) {
        if (dns.getHostByName(ntpServers[i], ntp_ip) == 1) {
            return true;
        }
    }

    // Fallback to Google NTP anycast IP (216.239.35.0)
    ntp_ip = IPAddress(216, 239, 35, 0); 
    return true;
}

static void lan_ntp_start() {
    // Cek link fisik DAN IP valid — jangan kirim UDP jika IP masih 0.0.0.0
    // (terjadi saat kabel baru dicolok tapi DHCP/static belum selesai)
    if (Ethernet.linkStatus() != LinkON) return;
    IPAddress myIP = Ethernet.localIP();
    if (myIP[0] == 0 && myIP[1] == 0 && myIP[2] == 0 && myIP[3] == 0) {
        Serial.println("[TIME] lan_ntp_start skipped: IP not yet assigned");
        return;
    }

    last_lan_sync_attempt_ms = millis();
    time_set_status("LAN NTP resolving", "LAN", true, false);

    if (!lan_ntp_resolve(lan_ntp_ip)) {
        lan_ntp_fail_count++;
        time_set_status("LAN NTP DNS failed", "LAN", false, false);
        Serial.println("[TIME] LAN NTP DNS failed");
        return;
    }

    udp.stop();
    udp.begin(8888);
    sendNTPpacket(lan_ntp_ip);
    lan_ntp_sent_ms = millis();
    lan_ntp_state = LanNtpState::WAIT_RESPONSE;
    time_set_status("LAN NTP waiting", "LAN", true, false);
    Serial.printf("[TIME] LAN NTP request sent to %u.%u.%u.%u\n",
                  lan_ntp_ip[0], lan_ntp_ip[1], lan_ntp_ip[2], lan_ntp_ip[3]);
}


static void lan_ntp_poll() {
    if (lan_ntp_state != LanNtpState::WAIT_RESPONSE) return;

    int packet_size = udp.parsePacket();
    if (packet_size >= NTP_PACKET_SIZE) {
        udp.read(packetBuffer, NTP_PACKET_SIZE);
        unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
        unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
        unsigned long secsSince1900 = (highWord << 16) | lowWord;
        const unsigned long seventyYears = 2208988800UL;
        unsigned long epoch = secsSince1900 - seventyYears;
        time_t now = epoch; // Set system clock in UTC
        struct timeval tv = { .tv_sec = now, .tv_usec = 0 };
        settimeofday(&tv, NULL);

        udp.stop();
        lan_ntp_state = LanNtpState::IDLE;
        lan_ntp_fail_count = 0;
        last_success_ms = millis();
        time_update_display_string("LAN");
        Serial.println("[TIME] LAN NTP Sync Success");
        return;
    }

    if (millis() - lan_ntp_sent_ms > LAN_NTP_RESPONSE_TIMEOUT_MS) {
        udp.stop();
        lan_ntp_state = LanNtpState::IDLE;
        lan_ntp_fail_count++;
        time_set_status("LAN NTP timeout", "LAN", false, false);
        Serial.println("[TIME] LAN NTP timeout");
    }
}

void time_manager_update() {
    bool manual = false;
    data_lock(g_state);
    manual = g_state.net.use_manual_time;
    data_unlock(g_state);

    static uint32_t last_debug = 0;
    if (millis() - last_debug > 2000) {
        last_debug = millis();
        bool wifi_connected = WiFi.status() == WL_CONNECTED;
        IPAddress lan_ip = Ethernet.localIP();
        bool lan_connected = (Ethernet.linkStatus() == LinkON) && 
                             (lan_ip[0] != 0 || lan_ip[1] != 0 || lan_ip[2] != 0 || lan_ip[3] != 0);
        Serial.printf("[TIME_DEBUG] manual=%d, wifi_conn=%d, lan_conn=%d (IP=%d.%d.%d.%d), linkStatus=%d, ntp_state=%d, last_success=%lu\n",
                      manual, wifi_connected, lan_connected, lan_ip[0], lan_ip[1], lan_ip[2], lan_ip[3],
                      (int)Ethernet.linkStatus(), (int)lan_ntp_state, last_success_ms);
    }

    static bool last_manual = false;
    if (manual != last_manual) {
        last_manual = manual;
        if (!manual) {
            // Manual -> NTP transition
            wifi_ntp_started = false;
            last_success_ms = 0;
            lan_ntp_state = LanNtpState::IDLE;
            lan_ntp_fail_count = 0;
            time_set_status("Waiting for NTP", "-", false, false);
            Serial.println("[TIME] Switched to NTP mode - triggering sync");
        } else {
            Serial.println("[TIME] Switched to Manual mode - skipping NTP");
        }
    }

    if (manual) {
        time_update_display_string("Manual");
        return;
    }

    lan_ntp_poll();

    bool wifi_connected = WiFi.status() == WL_CONNECTED;
    
    IPAddress lan_ip = Ethernet.localIP();
    bool lan_connected = (Ethernet.linkStatus() == LinkON) && 
                         (lan_ip[0] != 0 || lan_ip[1] != 0 || lan_ip[2] != 0 || lan_ip[3] != 0);

    if (!wifi_connected) {
        wifi_ntp_started = false;
    }

    if (wifi_connected && !wifi_ntp_started) {
        wifi_ntp_started = true;
        configTime(7 * 3600, 0, ntpServer);
        time_set_status("WiFi NTP waiting", "WiFi", true, false);
        Serial.println("[TIME] WiFi NTP start");
    }

    if (wifi_connected && time_update_display_string("WiFi")) {
        last_success_ms = millis();
        return;
    }

    if (!lan_connected) {
        if (rtc_time_valid) {
            time_update_display_string("RTC");
        }
        return;
    }

    uint32_t now = millis();
    bool never_synced = last_success_ms == 0;
    bool due_retry = now - last_lan_sync_attempt_ms > LAN_NTP_RETRY_MS;
    bool due_resync = now - last_success_ms > NTP_RESYNC_MS;

    if (lan_ntp_state == LanNtpState::IDLE && (never_synced || due_resync || due_retry)) {
        lan_ntp_start();
    }

    if (!wifi_connected) {
        if (!time_update_display_string("LAN") && rtc_time_valid) {
            time_update_display_string("RTC");
        }
    }
}

void time_manager_set_manual(int year, int month, int day, int hour, int minute) {
    struct tm t;
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = minute;
    t.tm_sec = 0;
    t.tm_isdst = -1;
    time_t epoch = mktime(&t);
    if (epoch != (time_t)-1) {
        struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        time_update_display_string("Manual");
        Serial.printf("[TIME] Manual time set to %04d-%02d-%02d %02d:%02d:00\n",
                      year, month, day, hour, minute);
    } else {
        Serial.println("[TIME] Failed to convert manual time fields to epoch!");
    }
}

