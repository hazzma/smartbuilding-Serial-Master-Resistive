/**
 * ============================================================
 * TEST MINIMAL: W5500 (SPI2) -> DHCP -> Internet -> NTP -> TLS MQTT (8883)
 * Board   : ESP32-S3 (board custom LAN + TFT)
 * Target  : EMQX Serverless (wd5de919.ala.asia-southeast1.emqxsl.com)
 *
 * Pin W5500:
 *   SCK  = 12  MISO = 13  MOSI = 11  CS = 10  RST = 18
 * ============================================================
 */

#include <Arduino.h>
#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include <Dns.h>
#include <PubSubClient.h>
#include <esp_mac.h>
#include <sys/time.h>

// --- ESP_SSLClient Configuration (Must be before header inclusion) ---
#define ENABLE_DEBUG
#define ENABLE_ERROR_STRING
#define DEBUG_PORT Serial
#include <ESP_SSLClient.h>

// --- Pin W5500 ---
#define LAN_SCK   12
#define LAN_MISO  13
#define LAN_MOSI  11
#define LAN_CS    10
#define LAN_RST   18

// --- MQTT EMQX Serverless ---
#define MQTT_SERVER  "wd5de919.ala.asia-southeast1.emqxsl.com"
#define MQTT_PORT    8883          // TLS/SSL Port
#define MQTT_USER    "Hansganteng"
#define MQTT_PASS    "12345678"
#define MQTT_TOPIC   "binus/ayam"

EthernetClient baseClient;
ESP_SSLClient  sslClient;
PubSubClient   mqttClient(sslClient);

// NTP via UDP manual
EthernetUDP udp;
static const int NTP_PACKET_SIZE = 48;
static byte      ntpBuf[NTP_PACKET_SIZE];

static void printIP(const char* label, IPAddress ip) {
    Serial.printf("[%s] %u.%u.%u.%u\n", label, ip[0], ip[1], ip[2], ip[3]);
}

static bool ipValid(IPAddress ip) {
    return (ip[0] | ip[1] | ip[2] | ip[3]) != 0;
}

// =====================================================
// STEP 1: Init W5500
// =====================================================
static bool step_init_w5500() {
    Serial.println("\n=== STEP 1: Init W5500 (SPI2 Hardware Reset) ===");

    pinMode(LAN_RST, OUTPUT);
    digitalWrite(LAN_RST, LOW);
    delay(100);
    digitalWrite(LAN_RST, HIGH);
    delay(200);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    Serial.printf("[W5500] MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Force default SPI object to use our W5500 SPI2 pins
    SPI.begin(LAN_SCK, LAN_MISO, LAN_MOSI, LAN_CS);
    Ethernet.init(LAN_CS);

    // DHCP otomatis (timeout 12s connect, 6s response)
    Serial.println("[W5500] Trying DHCP...");
    if (Ethernet.begin(mac, 12000, 6000) != 0) {
        printIP("DHCP OK - IP    ", Ethernet.localIP());
        printIP("DHCP OK - GW    ", Ethernet.gatewayIP());
        printIP("DHCP OK - DNS   ", Ethernet.dnsServerIP());
        return true;
    }

    // Static IP fallback
    Serial.println("[W5500] DHCP failed -> Static IP fallback...");
    IPAddress staticIP(192, 168, 1, 177);
    IPAddress gateway (192, 168, 1,   1);
    IPAddress subnet  (255, 255, 255,  0);
    IPAddress dns     (8,   8,   8,    8);
    Ethernet.begin(mac, staticIP, dns, gateway, subnet);
    delay(300);

    if (ipValid(Ethernet.localIP())) {
        printIP("Static IP OK - IP", Ethernet.localIP());
        return true;
    }

    Serial.println("[W5500] GAGAL TOTAL - Check W5500 hardware wiring/power");
    return false;
}

// =====================================================
// STEP 2: Cek internet (DNS resolve + TCP connect)
// =====================================================
static bool step_check_internet() {
    Serial.println("\n=== STEP 2: Cek Internet (DNS + TCP:80) ===");

    DNSClient dns;
    dns.begin(Ethernet.dnsServerIP());
    IPAddress googleIP;

    Serial.print("[DNS] Resolving google.com... ");
    if (dns.getHostByName("google.com", googleIP) != 1) {
        Serial.println("FAILED -> Trying 8.8.8.8");
        dns.begin(IPAddress(8, 8, 8, 8));
        if (dns.getHostByName("google.com", googleIP) != 1) {
            Serial.println("[DNS] DNS failed completely");
            return false;
        }
    }
    printIP("DNS OK - google.com", googleIP);

    Serial.print("[TCP] Connecting to google.com:80... ");
    EthernetClient testClient;
    if (testClient.connect(googleIP, 80)) {
        Serial.println("CONNECTED -> Internet Access OK");
        testClient.stop();
        return true;
    }
    Serial.println("FAILED -> Gateway routing issue?");
    return false;
}

// =====================================================
// STEP 3: NTP Sync via UDP
// =====================================================
static bool step_ntp_sync() {
    Serial.println("\n=== STEP 3: NTP Sync via UDP ===");

    const char* ntpServers[] = {
        "pool.ntp.org",
        "time.google.com",
        "time.windows.com",
        "time.nist.gov"
    };
    IPAddress ntpIP;
    bool resolved = false;

    DNSClient dns;
    dns.begin(Ethernet.dnsServerIP());

    for (int i = 0; i < 4; i++) {
        Serial.printf("[NTP] Resolving %s... ", ntpServers[i]);
        if (dns.getHostByName(ntpServers[i], ntpIP) == 1) {
            printIP("Resolved", ntpIP);
            resolved = true;
            break;
        }
        Serial.println("Failed");
    }

    if (!resolved) {
        ntpIP = IPAddress(216, 239, 35, 0); // time.google.com fallback IP
        Serial.println("[NTP] DNS failed. Falling back to Google NTP IP: 216.239.35.0");
    }

    udp.stop();
    if (udp.begin(8888) == 0) {
        Serial.println("[NTP] Failed to bind local UDP port 8888");
        return false;
    }

    bool gotResponse = false;
    for (int retry = 0; retry < 3; retry++) {
        Serial.printf("[NTP] Sending packet (attempt %d)... ", retry + 1);
        memset(ntpBuf, 0, NTP_PACKET_SIZE);
        ntpBuf[0] = 0b11100011;   // LI, Version, Mode
        ntpBuf[1] = 0;
        ntpBuf[2] = 6;
        ntpBuf[3] = 0xEC;
        ntpBuf[12] = 49; ntpBuf[13] = 0x4E; ntpBuf[14] = 49; ntpBuf[15] = 52;
        
        if (udp.beginPacket(ntpIP, 123) == 0) {
            Serial.println("beginPacket failed");
            delay(1000);
            continue;
        }
        udp.write(ntpBuf, NTP_PACKET_SIZE);
        if (udp.endPacket() == 0) {
            Serial.println("endPacket failed");
            delay(1000);
            continue;
        }
        
        uint32_t t0 = millis();
        while (millis() - t0 < 3000) {
            int packetSize = udp.parsePacket();
            if (packetSize >= NTP_PACKET_SIZE) {
                udp.read(ntpBuf, NTP_PACKET_SIZE);
                uint32_t hi  = word(ntpBuf[40], ntpBuf[41]);
                uint32_t lo  = word(ntpBuf[42], ntpBuf[43]);
                uint32_t sec = ((hi << 16) | lo) - 2208988800UL;
                
                // Set system time (add 7 hours for WIB GMT+7)
                struct timeval tv = { .tv_sec = (time_t)(sec + 7 * 3600), .tv_usec = 0 };
                settimeofday(&tv, NULL);
                
                struct tm ti;
                getLocalTime(&ti);
                Serial.printf("SUCCESS! Time: %02d:%02d:%02d (%04d-%02d-%02d)\n",
                              ti.tm_hour, ti.tm_min, ti.tm_sec,
                              ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday);
                gotResponse = true;
                udp.stop();
                return true;
            }
            delay(10);
        }
        Serial.println("Timeout");
        delay(1000);
    }

    udp.stop();
    Serial.println("[NTP] GAGAL - NTP server did not respond");
    return false;
}

// =====================================================
// STEP 4: MQTT Connect + Publish (TLS/SSL)
// =====================================================
static void mqtt_callback(char* topic, byte* payload, unsigned int len) {
    Serial.printf("[MQTT] Message arrived on [%s] : ", topic);
    for (unsigned int i = 0; i < len; i++) Serial.print((char)payload[i]);
    Serial.println();
}

static bool step_mqtt_connect() {
    Serial.println("\n=== STEP 4: MQTT ke EMQX via TLS (Port 8883) ===");
    Serial.printf("[MQTT] Server: %s:%d | User: %s\n", MQTT_SERVER, MQTT_PORT, MQTT_USER);

    DNSClient dns;
    dns.begin(Ethernet.dnsServerIP());
    IPAddress brokerIP;
    Serial.print("[MQTT] DNS resolving broker domain... ");
    if (dns.getHostByName(MQTT_SERVER, brokerIP) != 1) {
        Serial.println("FAILED");
        return false;
    }
    printIP("Broker IP", brokerIP);

    // Setup SSL client wrapper
    sslClient.setClient(&baseClient);
    sslClient.setInsecure(); // Skip certificate checks for testing
    sslClient.setBufferSizes(16384, 2048); // Standard TLS Rx fragment (16KB)
    sslClient.setDebugLevel(3); // Verbose log (info level)

    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
    mqttClient.setCallback(mqtt_callback);
    mqttClient.setKeepAlive(60);
    mqttClient.setSocketTimeout(15);

    String cid = "MasterS3-" + String(random(0xffff), HEX);
    Serial.printf("[MQTT] Client ID: %s\n", cid.c_str());
    Serial.print("[MQTT] Connecting... ");

    if (mqttClient.connect(cid.c_str(), MQTT_USER, MQTT_PASS)) {
        Serial.println("CONNECTED!");
        mqttClient.subscribe(MQTT_TOPIC);

        const char* msg = "{\"status\":\"W5500 TLS MQTT CONNECTED\",\"board\":\"ESP32-S3\"}";
        bool pub = mqttClient.publish(MQTT_TOPIC, msg);
        Serial.printf("[MQTT] Publish: %s\n", pub ? "OK" : "FAILED");
        return true;
    }

    int rc = mqttClient.state();
    Serial.printf("FAILED rc=%d\n", rc);
    return false;
}

// =====================================================
// Setup & Loop
// =====================================================
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n╔══════════════════════════════════════════════╗");
    Serial.println("║  W5500 LAN SECURE DIAGNOSTIC TEST v1.2       ║");
    Serial.println("╚══════════════════════════════════════════════╝");

    if (!step_init_w5500()) {
        Serial.println("\n[FATAL] W5500 failed to init. System halted.");
        while (true) delay(1000);
    }

    bool internet = step_check_internet();
    if (!internet) {
        Serial.println("[WARN] Internet connectivity test failed.");
    }

    bool ntp  = step_ntp_sync();
    bool mqtt = step_mqtt_connect();

    Serial.println("\n╔══════════════════════════════════════════════╗");
    Serial.println("║                DIAGNOSTIC RESULT             ║");
    Serial.println("╠══════════════════════════════════════════════╣");
    Serial.printf( "║  W5500 Init + DHCP   : OK                    ║\n");
    Serial.printf( "║  Internet Ping (TCP) : %-6s                ║\n", internet ? "OK" : "FAILED");
    Serial.printf( "║  NTP Sync (UDP)      : %-6s                ║\n", ntp      ? "OK" : "FAILED");
    Serial.printf( "║  MQTT TLS (Port 8883): %-6s                ║\n", mqtt     ? "OK" : "FAILED");
    Serial.println("╚══════════════════════════════════════════════╝");
}

void loop() {
    if (mqttClient.connected()) {
        mqttClient.loop();
    }

    static uint32_t last_print = 0;
    if (millis() - last_print > 1000) {
        last_print = millis();
        
        struct tm ti;
        bool time_ok = getLocalTime(&ti, 10);
        
        Serial.printf("[CLOCK] Time: %02d:%02d:%02d (%04d-%02d-%02d) | MQTT: %s\n",
                      time_ok ? ti.tm_hour : 0, 
                      time_ok ? ti.tm_min : 0, 
                      time_ok ? ti.tm_sec : 0,
                      time_ok ? (ti.tm_year + 1900) : 0, 
                      time_ok ? (ti.tm_mon + 1) : 0, 
                      time_ok ? ti.tm_mday : 0,
                      mqttClient.connected() ? "CONNECTED" : "DISCONNECTED");
    }

    static uint32_t last_heartbeat = 0;
    if (mqttClient.connected() && (millis() - last_heartbeat > 10000)) {
        last_heartbeat = millis();
        char msg[64];
        snprintf(msg, sizeof(msg), "{\"uptime_ms\":%lu,\"heap\":%u}", millis(), ESP.getFreeHeap());
        bool ok = mqttClient.publish(MQTT_TOPIC, msg);
        Serial.printf("[MQTT] Heartbeat %s: %s\n", ok ? "OK" : "FAIL", msg);
    }

    if (!mqttClient.connected()) {
        static uint32_t last_reconnect = 0;
        if (millis() - last_reconnect > 10000) {
            last_reconnect = millis();
            Serial.println("[MQTT] Connection lost. Reconnecting...");
            step_mqtt_connect();
        }
    }
}
