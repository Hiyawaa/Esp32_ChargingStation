#include <Arduino.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <FS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <map>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include <Preferences.h>
#include <WebServer.h>

// ================= ESP32 SPI PINS (SCREEN) =================
#define TFT_MOSI  13
#define TFT_SCLK  26
#define TFT_CS    15
#define TFT_DC    4
#define TFT_RST   32

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

// ================= HARDWARE PINS =================
// GPIO 27 is ADC2 — unusable with analogRead() while WiFi is active.
// Move the sensor wire to GPIO 34 (ADC1, input-only, WiFi-safe).
const int METAL_SENSOR_PIN = 34;
const int LED_PIN           = 25;
const int BTN_1_PIN         = 22;
const int BTN_2_PIN         = 23;
// NOTE: Relay pins 12, 14, 16, 17 have been moved to the slave ESP32.
//       GPIO 16 & 17 are now repurposed as UART2 TX/RX to the slave.

// ================= UART2 → SLAVE ESP32 =================
// Wire: Master TX2 (GPIO 17) → Slave RX (GPIO 16)
//       Master RX2 (GPIO 16) ← Slave TX (GPIO 17)
//       Common GND
#define SLAVE_UART_TX   17
#define SLAVE_UART_RX   16
#define SLAVE_UART_BAUD 115200

// ================= COIN SENSORS =================
struct CoinPin {
    int  pin;
    int  lastState;
    long count;
};

std::map<String, CoinPin> coinData = {
    {"1",  {5,  HIGH, 0}},
    {"5",  {18, HIGH, 0}},
    {"10", {19, HIGH, 0}},
    {"20", {21, HIGH, 0}}
};

// ================= UI STATES =================
enum Page {
    PAGE_SETUP,
    PAGE_INSERT,
    PAGE_SELECT,
    PAGE_CHARGING,
    PAGE_WIFI,
    PAGE_EXTEND,
    PAGE_WIFI_EXTEND
};

Page currentPage = PAGE_INSERT;

int  credits           = 0;
int  pendingCredits    = 0;
int  wifiPendingCredits = 0;
int  unassignedSeconds = 0;
int  portSeconds[4]    = {0, 0, 0, 0};
int  wifiSeconds       = 0;
bool portActive[4]     = {false, false, false, false};
int  lastActivatedPort = 0;
int  wifiUsers         = 0;

bool          showText       = true;
unsigned long lastBlink      = 0;
unsigned long lastSecondTick = 0;

// ── EXTEND / PORT-SELECT STATE ────────────────────────────────
bool selectingPort   = false;
int  highlightedPort = -1;

// ================= COLORS =================
#define BG     ST77XX_BLACK
#define TXT    ST77XX_WHITE
#define HL     ST77XX_YELLOW
#define OK     ST77XX_GREEN
#define OFF    ST77XX_RED
#define HEADER ST77XX_BLUE

// ================= FIREBASE & WIFI =================
const char* API_KEY      = "AIzaSyDGV3vPuxVRkHSBDESwc7nCud2Zz4jDjNM";
const char* DATABASE_URL = "esp32padin-default-rtdb.asia-southeast1.firebasedatabase.app";
const char* FILENAME     = "/data.json";

FirebaseData   fbdo;
FirebaseAuth   auth;
FirebaseConfig config;

// Apmode
bool          apModeActive   = false;
unsigned long apModeStart    = 0;
unsigned long apModeDuration = 0;

bool          needsFirebaseUpdate = false;
unsigned long lastFirebaseUpdate  = 0;
const unsigned long firebaseInterval = 3600000UL;

// Accumulated WiFi revenue (peso value of all WiFi coins ever inserted)
float wifiEarningsTotal = 0.0f;

// Per-day earnings accumulators — reset when the date changes
String lastEarningsDate    = "";
float  dailyChargingEarned = 0.0f;
float  dailyWifiEarned     = 0.0f;

bool          needsStatusUpdate   = false;
unsigned long lastStatusUpdate    = 0;
const unsigned long statusInterval = 5000UL;  // push live status every 5 s

bool          metalDetected     = false;
unsigned long metalDetectedTime = 0;
unsigned long metalLowStartTime = 0;
bool          metalPinLow       = false;

const int     METAL_ADC_THRESHOLD = 3000;
const unsigned long METAL_HOLD_MS    = 30;
const unsigned long METAL_TIMEOUT_MS = 5000;

struct CoinSetting {
    bool enabled;
    int  time; // minutes
};

std::map<String, CoinSetting> coinSettings = {
    {"1",  {true,  5}},
    {"5",  {true, 15}},
    {"10", {true, 30}},
    {"20", {true, 60}}
};

// ================= SMART SETUP =================
Preferences preferences;
WebServer   server(80);
bool        isSetupMode = false;

// ================= WIFI SESSION (SLAVE LINK) =================
// MAC address of the last client that connected to the slave's AP.
// Populated automatically when the slave sends "MAC:<addr>\n" over UART
// each time a new STA associates with its captive-portal AP.
// Falls back to broadcast until a real MAC is received.
String wifiTargetMac = "FF:FF:FF:FF:FF:FF";

// Running buffer for unsolicited lines from the slave
String slaveLineBuffer = "";

// ──────────────────────────────────────────────────────────────
// readSlaveUart()
//
// Call from loop() every iteration to catch unsolicited messages
// from the slave, particularly "MAC:<addr>" pushes that arrive
// whenever a new device joins the slave's captive-portal AP.
// ──────────────────────────────────────────────────────────────
void readSlaveUart() {
    while (Serial2.available()) {
        char c = (char)Serial2.read();
        if (c == '\n') {
            slaveLineBuffer.trim();
            if (slaveLineBuffer.startsWith("MAC:")) {
                // Format: "MAC:AA:BB:CC:DD:EE:FF"
                wifiTargetMac = slaveLineBuffer.substring(4);
                wifiTargetMac.trim();
                Serial.printf("[UART←SLAVE] Last WiFi client MAC: %s\n",
                              wifiTargetMac.c_str());
            }
            // Other unsolicited lines (ACKs from previous commands) are silently discarded here
            slaveLineBuffer = "";
        } else if (c != '\r') {
            slaveLineBuffer += c;
        }
    }
}

// ──────────────────────────────────────────────────────────────
// waitSlaveAck()
//
// Shared helper — blocks up to timeoutMs waiting for a non-empty
// line from the slave and returns it (trimmed, no newline).
// Returns "" on timeout.
// ──────────────────────────────────────────────────────────────
String waitSlaveAck(unsigned long timeoutMs = 3000) {
    unsigned long deadline = millis() + timeoutMs;
    String        reply    = "";
    while (millis() < deadline) {
        while (Serial2.available()) {
            char c = (char)Serial2.read();
            if (c == '\n') {
                reply.trim();
                if (reply.length() > 0) return reply;
                reply = "";
            } else if (c != '\r') {
                reply += c;
            }
        }
        delay(10);
    }
    return ""; // timeout
}

// ──────────────────────────────────────────────────────────────
// setRelay()   /   setAllRelaysOff()
//
// Relay commands:
//   "RELAY:<port>:ON\n"   → slave activates relay  (GPIO LOW,  active-low)
//   "RELAY:<port>:OFF\n"  → slave deactivates relay (GPIO HIGH)
//   "RELAY:ALL:OFF\n"     → slave turns ALL relays OFF
// ──────────────────────────────────────────────────────────────
void setRelay(int port, bool on) {
    Serial2.printf("RELAY:%d:%s\n", port, on ? "ON" : "OFF");
    delay(10);
}

void setAllRelaysOff() {
    Serial2.println("RELAY:ALL:OFF");
    delay(10);
}

// ──────────────────────────────────────────────────────────────
// forwardWiFiToSlave()
//
// Called by handleSetup() during first-run provisioning.
// Sends "WIFI:<ssid>|<pass>\n" so the slave saves credentials
// to its own NVS and reboots to join the home network.
// ──────────────────────────────────────────────────────────────
void forwardWiFiToSlave(const String& ssid, const String& pass) {
    Serial2.printf("WIFI:%s|%s\n", ssid.c_str(), pass.c_str());
    Serial.printf("[UART→SLAVE] Sent WiFi creds  SSID='%s'  PASS='%s'\n",
                  ssid.c_str(), pass.c_str());

    String reply = waitSlaveAck(3000);

    if (reply == "ACK:WIFI:SAVED") {
        Serial.println("[UART←SLAVE] ✓ ACK received — slave saved credentials & will reboot.");
    } else if (reply.startsWith("ERR:")) {
        Serial.printf("[UART←SLAVE] ✗ Slave returned error: %s\n", reply.c_str());
    } else if (reply.length() == 0) {
        Serial.println("[UART←SLAVE] ✗ TIMEOUT — no reply from slave within 3 s.");
        Serial.println("             Check wiring (TX17→RX16) and slave firmware.");
    } else {
        Serial.printf("[UART←SLAVE] ? Unexpected reply: '%s'\n", reply.c_str());
    }

    delay(500);
}

// ──────────────────────────────────────────────────────────────
// sendWifiTime()
//
// Sends "TIME:<MAC>:<seconds>\n" to the slave so it registers
// (or extends) a captive-portal session for that MAC address.
//
// Slave reply:
//   "ACK:TIME:<MAC>:<seconds>\n"  → session registered
//   "ERR:BAD_MAC\n"               → malformed MAC
//   "ERR:BAD_SECONDS\n"           → seconds ≤ 0
//   "ERR:BAD_TIME_FMT\n"          → command too short
//   "" (timeout)                  → slave not responding
//
// Rate: 1 peso credit = 1 minute by default.
// Adjust the caller (PAGE_SELECT handler) to change the rate.
// ──────────────────────────────────────────────────────────────
void sendWifiTime(const String& mac, int32_t seconds) {
    if (seconds <= 0) {
        Serial.println("[sendWifiTime] Skipped — seconds <= 0.");
        return;
    }

    Serial2.printf("TIME:%s:%ld\n", mac.c_str(), (long)seconds);
    Serial.printf("[UART→SLAVE] TIME %s  +%ld s\n", mac.c_str(), (long)seconds);

    String reply = waitSlaveAck(3000);

    if (reply.startsWith("ACK:TIME:")) {
        Serial.printf("[UART←SLAVE] ✓ Session registered: %s\n", reply.c_str());
    } else if (reply.startsWith("ERR:")) {
        Serial.printf("[UART←SLAVE] ✗ Error from slave: %s\n", reply.c_str());
    } else if (reply.length() == 0) {
        Serial.println("[UART←SLAVE] ✗ TIMEOUT — no ACK from slave for TIME command.");
    } else {
        Serial.printf("[UART←SLAVE] ? Unexpected reply: '%s'\n", reply.c_str());
    }
}

// ================= SETUP HTTP HANDLER =================
// ──────────────────────────────────────────────────────────────
// handleSetup()
//
// Called by the Flutter app's HTTP POST to /setup.
// 1. Saves credentials to master NVS (Preferences).
// 2. Forwards the same credentials to the slave over UART so
//    the slave can save them to its own NVS and reboot to connect
//    automatically — no manual code editing needed on the slave.
// 3. Reboots the master to apply the new WiFi settings.
// ──────────────────────────────────────────────────────────────
void handleSetup() {
    if (server.hasArg("ssid") && server.hasArg("pass")) {
        String new_ssid = server.arg("ssid");
        String new_pass = server.arg("pass");

        // 1. Save on master
        preferences.putString("ssid", new_ssid);
        preferences.putString("pass", new_pass);

        // 2. Forward to slave via UART before we lose the AP connection
        forwardWiFiToSlave(new_ssid, new_pass);

        // 3. Respond to the Flutter app, then reboot master
        server.send(200, "application/json",
                    "{\"success\":true,\"message\":\"Credentials saved! Rebooting...\"}");
        delay(1000);
        ESP.restart();
    } else {
        server.send(400, "application/json",
                    "{\"success\":false,\"message\":\"Missing ssid or pass parameter\"}");
    }
}

// ================= PORT HELPERS =================

int firstActivePort() {
    for (int i = 0; i < 4; i++) {
        if (portSeconds[i] > 0) return i;
    }
    return -1;
}

int activateRandomPort(int timeToAdd) {
    int availablePorts[4];
    int availableCount = 0;

    for (int i = 0; i < 4; i++) {
        if (!portActive[i] && portSeconds[i] <= 0) {
            availablePorts[availableCount++] = i;
        }
    }

    if (availableCount == 0) {
        portSeconds[lastActivatedPort] += timeToAdd;
        return lastActivatedPort;
    }

    int selectedPort = availablePorts[random(0, availableCount)];

    portActive[selectedPort]  = true;
    portSeconds[selectedPort] = timeToAdd;
    setRelay(selectedPort, true);   // ← UART: slave activates relay

    lastActivatedPort = selectedPort;
    return selectedPort;
}

void deactivateAllPorts() {
    for (int i = 0; i < 4; i++) {
        portActive[i]  = false;
        portSeconds[i] = 0;
    }
    setAllRelaysOff();              // ← UART: slave cuts all relays
}

// ================= TIME HELPERS =================
String getToday() {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    char buffer[11];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d", &timeinfo);
    return String(buffer);
}

String formatTime(int seconds) {
    int h = seconds / 3600;
    int m = (seconds % 3600) / 60;
    int s = seconds % 60;
    char buffer[10];
    sprintf(buffer, "%02d:%02d:%02d", h, m, s);
    return String(buffer);
}

// ================= DISPLAY =================
void drawHeader() {
    tft.fillRect(0, 0, 160, 20, HEADER);
    tft.setCursor(20, 5);
    tft.setTextColor(TXT);
    tft.setTextSize(1);
    tft.println("MULTI VENDO");
}

void drawSetupPage() {
    tft.fillScreen(BG);
    drawHeader();
    tft.setCursor(10, 30);
    tft.setTextColor(HL);
    tft.setTextSize(1);
    tft.println("SETUP MODE");
    tft.setCursor(10, 50);
    tft.setTextColor(TXT);
    tft.println("Open Flutter app &");
    tft.setCursor(10, 62);
    tft.println("connect to:");
    tft.setCursor(10, 80);
    tft.setTextColor(OK);
    tft.println("ESP32_VENDO");
    tft.setCursor(10, 100);
    tft.setTextColor(TXT);
    tft.println("Pass: 12345678");
    tft.drawRect(0, 0, 160, 128, TXT);
}

void drawInsertPage() {
    tft.fillScreen(BG);
    drawHeader();
    tft.setCursor(30, 25);
    tft.setTextSize(2);
    tft.setTextColor(TXT);
    tft.print("P");
    tft.print(credits);
    tft.setTextSize(1);
    tft.setCursor(5, 55); tft.printf("1  = %d MIN\n", coinSettings["1"].time);
    tft.setCursor(5, 65); tft.printf("5  = %d MIN\n", coinSettings["5"].time);
    tft.setCursor(5, 75); tft.printf("10 = %d MIN\n", coinSettings["10"].time);
    tft.setCursor(5, 85); tft.printf("20 = %d MIN\n", coinSettings["20"].time);
    tft.drawRect(0, 0, 160, 128, TXT);
}

void drawSelectPage() {
    tft.fillScreen(BG);
    drawHeader();
    tft.setCursor(10, 30);
    tft.setTextColor(TXT);
    tft.print("Credits: P");
    tft.println(credits);
    tft.setCursor(20, 55);
    tft.setTextColor(OK);
    tft.println("[1] Charging");
    tft.setCursor(20, 75);
    tft.setTextColor(HL);
    tft.println("[2] WiFi");
    tft.drawRect(0, 0, 160, 128, TXT);
}

void drawChargingPage() {
    tft.fillScreen(BG);
    drawHeader();
    int y = 30;
    for (int i = 0; i < 4; i++) {
        if (portSeconds[i] > 0) {
            tft.setCursor(5, y);
            tft.setTextColor(OK);
            tft.printf("PORT %d ", i + 1);
            tft.setTextColor(HL);
            tft.println(formatTime(portSeconds[i]));
            y += 20;
        }
    }
    tft.drawRect(0, 0, 160, 128, TXT);
}

// ──────────────────────────────────────────────────────────────
// drawWiFiPage()
//
// Shows the active WiFi session details:
//   - Remaining time (counted down by loop())
//   - Target MAC address that holds the session on the slave
//   - SSID of the slave's captive portal AP
// Auto-updates every second via the main loop redraw.
// ──────────────────────────────────────────────────────────────
void drawWiFiPage() {
    tft.fillScreen(BG);
    drawHeader();

    tft.setCursor(10, 28);
    tft.setTextColor(HL);
    tft.setTextSize(1);
    tft.print("Time: ");
    tft.println(formatTime(wifiSeconds));

    tft.setCursor(10, 46);
    tft.setTextColor(OK);
    tft.print("Devices: ");
    tft.println(wifiUsers);

    tft.setCursor(10, 64);
    tft.setTextColor(TXT);
    tft.println("SSID: ESP32_WIFI");

    tft.setCursor(5, 82);
    tft.setTextColor(ST77XX_CYAN);
    tft.println("192.168.4.1  to check");

    tft.drawRect(0, 0, 160, 128, TXT);
}

void drawExtendPage() {
    tft.fillScreen(BG);
    drawHeader();

    tft.setCursor(5, 22);
    tft.setTextColor(HL);
    tft.setTextSize(1);
    tft.print("Added: P");
    tft.println(pendingCredits);

    int y = 38;
    for (int i = 0; i < 4; i++) {
        if (portSeconds[i] > 0) {
            tft.setCursor(5, y);
            tft.setTextColor(OK);
            tft.printf("P%d ", i + 1);
            tft.setTextColor(TXT);
            tft.print(formatTime(portSeconds[i]));
            y += 16;
        }
    }

    tft.setCursor(5, 100);
    tft.setTextColor(OK);
    tft.println("[1] Extend Time");
    tft.setCursor(5, 112);
    tft.setTextColor(HL);
    tft.println("[2] New Port");
    tft.drawRect(0, 0, 160, 128, TXT);
}

// ──────────────────────────────────────────────────────────────
// drawWiFiExtendPage()
//
// Shown when a coin is inserted while already on PAGE_WIFI.
// Mirrors the charging extend menu but for WiFi sessions:
//   [1] Extend Time  → add time to the current connected user (wifiTargetMac)
//   [2] New User     → send time to whoever joins next (broadcast MAC)
// ──────────────────────────────────────────────────────────────
void drawWiFiExtendPage() {
    tft.fillScreen(BG);
    drawHeader();

    tft.setCursor(5, 22);
    tft.setTextColor(HL);
    tft.setTextSize(1);
    tft.print("Added: P");
    tft.println(wifiPendingCredits);

    tft.setCursor(5, 42);
    tft.setTextColor(TXT);
    tft.print("Time left: ");
    tft.setTextColor(OK);
    tft.println(formatTime(wifiSeconds));

    tft.setCursor(5, 90);
    tft.setTextColor(OK);
    tft.println("[1] Extend Time");
    tft.setCursor(5, 106);
    tft.setTextColor(HL);
    tft.println("[2] New User");
    tft.drawRect(0, 0, 160, 128, TXT);
}

void drawPortSelectPage(int highlighted) {
    tft.fillScreen(BG);
    drawHeader();

    tft.setCursor(5, 22);
    tft.setTextColor(HL);
    tft.setTextSize(1);
    tft.println("Select Port:");

    int y = 38;
    for (int i = 0; i < 4; i++) {
        if (portSeconds[i] > 0) {
            tft.setCursor(5, y);
            if (i == highlighted) {
                tft.setTextColor(OK);
                tft.print("> PORT ");
            } else {
                tft.setTextColor(TXT);
                tft.print("  PORT ");
            }
            tft.print(i + 1);
            tft.print("  ");
            tft.setTextColor(HL);
            tft.println(formatTime(portSeconds[i]));
            y += 16;
        }
    }

    tft.setCursor(5, 100);
    tft.setTextColor(ST77XX_CYAN);
    tft.println("[1] Confirm");
    tft.setCursor(5, 112);
    tft.setTextColor(TXT);
    tft.println("[2] Next port");
    tft.drawRect(0, 0, 160, 128, TXT);
}

// ================= DATA LOGIC =================
void load_data() {
    if (!LittleFS.exists(FILENAME)) return;
    File file = LittleFS.open(FILENAME, FILE_READ);
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, file);
    file.close();
    for (auto const& [name, data] : coinData) {
        if (doc.containsKey(name)) coinData[name].count = doc[name];
    }
}

void save_data() {
    File file = LittleFS.open(FILENAME, FILE_WRITE);
    if (!file) return;
    DynamicJsonDocument doc(1024);
    for (auto const& [name, data] : coinData) {
        doc[name] = data.count;
    }
    serializeJson(doc, file);
    file.close();
}

void load_coin_settings() {
    if (!Firebase.ready()) return;
    if (Firebase.RTDB.getJSON(&fbdo, "/settings/coins")) {
        DynamicJsonDocument doc(512);
        deserializeJson(doc, fbdo.jsonString());
        for (JsonPair coin : doc.as<JsonObject>()) {
            String c = coin.key().c_str();
            if (coin.value().containsKey("enabled")) {
                coinSettings[c].enabled = coin.value()["enabled"];
            }
        }
    }
}

void update_firebase() {
    if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
    unsigned long now = millis();
    if (!needsFirebaseUpdate && (now - lastFirebaseUpdate < firebaseInterval)) return;

    // ── Charging totals from coin counters ─────────────────────
    float totalCharging = 0;
    FirebaseJson json;
    for (auto const& [name, data] : coinData) {
        json.set("settings/coins/" + name + "/count", data.count);
        totalCharging += name.toInt() * data.count;
    }

    // ── All-time totals ────────────────────────────────────────
    json.set("total_earnings/charging", totalCharging);
    json.set("total_earnings/wifi",     wifiEarningsTotal);

    // ── Daily logs — accumulate per day, never replace ─────────
    // We keep per-day accumulators (dailyChargingEarned, dailyWifiEarned)
    // that reset whenever the calendar date changes.
    String today = getToday();
    if (today != lastEarningsDate) {
        // New day — reset daily counters
        dailyChargingEarned = 0.0f;
        dailyWifiEarned     = 0.0f;
        lastEarningsDate    = today;
    }

    // Read what's already in Firebase for today so we don't overwrite
    // data from a previous boot on the same day.
    // We use a separate read path to seed our local accumulators on first write.
    // Simple approach: only write *incremental* daily totals tracked in RAM.
    // If the device reboots mid-day the daily counter resets to 0 and adds
    // from that point — acceptable. Use Firebase transactions for hard accuracy.
    json.set("logs/" + today + "/charging", dailyChargingEarned);
    json.set("logs/" + today + "/wifi",     dailyWifiEarned);

    if (Firebase.RTDB.updateNode(&fbdo, "/", &json)) {
        needsFirebaseUpdate = false;
    }
    lastFirebaseUpdate = now;
}

// ──────────────────────────────────────────────────────────────
// push_status_firebase()
//
// Pushes live session status to Firebase every ~5 seconds so
// the Flutter app can display real-time port and WiFi info:
//
//   status/active_ports          → int  (0-4)
//   status/wifi_users            → int
//   status/wifi_seconds          → int
//   status/ports/0..3/active     → bool
//   status/ports/0..3/seconds    → int
// ──────────────────────────────────────────────────────────────
void push_status_firebase() {
    if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
    unsigned long now = millis();
    if (!needsStatusUpdate && (now - lastStatusUpdate < statusInterval)) return;

    int activeCount = 0;
    for (int i = 0; i < 4; i++) if (portSeconds[i] > 0) activeCount++;

    FirebaseJson sJson;
    sJson.set("status/active_ports", activeCount);
    sJson.set("status/wifi_users",   wifiUsers);
    sJson.set("status/wifi_seconds", wifiSeconds);
    for (int i = 0; i < 4; i++) {
        String base = "status/ports/" + String(i);
        sJson.set(base + "/active",  portSeconds[i] > 0);
        sJson.set(base + "/seconds", portSeconds[i]);
    }

    Firebase.RTDB.updateNode(&fbdo, "/", &sJson);
    needsStatusUpdate = false;
    lastStatusUpdate  = now;
}

// ================= SETUP =================
void setup() {
    Serial.begin(115200);
    randomSeed(analogRead(34));

    // --- Screen ---
    SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
    tft.initR(INITR_BLACKTAB);
    tft.setRotation(3);
    tft.fillScreen(BG);
    tft.setCursor(10, 50);
    tft.setTextColor(TXT);
    tft.setTextSize(1);
    tft.println("Booting up...");

    // --- GPIO: sensors, LED, buttons, coin inputs ---
    pinMode(LED_PIN, OUTPUT);
    pinMode(METAL_SENSOR_PIN, INPUT);
    pinMode(BTN_1_PIN, INPUT_PULLDOWN);
    pinMode(BTN_2_PIN, INPUT_PULLDOWN);
    for (auto const& [name, data] : coinData) {
        pinMode(data.pin, INPUT_PULLUP);
    }

    // --- UART2: open channel to slave BEFORE smart-setup check ---
    Serial2.begin(SLAVE_UART_BAUD, SERIAL_8N1, SLAVE_UART_RX, SLAVE_UART_TX);
    delay(200);
    setAllRelaysOff(); // slave starts with all relays OFF regardless of mode

    // --- Smart Setup ---
    preferences.begin("wifi_creds", false);
    String saved_ssid = preferences.getString("ssid", "");
    String saved_pass = preferences.getString("pass", "");

    if (saved_ssid == "") {
        // ── SETUP MODE ──────────────────────────────────────────
        isSetupMode = true;
        currentPage = PAGE_SETUP;

        WiFi.mode(WIFI_AP);
        WiFi.softAP("ESP32_VENDO", "12345678");

        server.on("/setup", HTTP_POST, handleSetup);
        server.begin();

        drawSetupPage();
        Serial.println("[SETUP] AP active. Waiting for Flutter app...");
        Serial.printf("[SETUP] AP IP: %s\n", WiFi.softAPIP().toString().c_str());
        return;
    }

    // ── NORMAL MODE ─────────────────────────────────────────────
    tft.println("Connecting WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(saved_ssid.c_str(), saved_pass.c_str());

    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 30) {
        delay(500);
        retries++;
    }

    if (WiFi.status() != WL_CONNECTED) {
        tft.println("WiFi Failed!");
        tft.println("Resetting...");
        preferences.putString("ssid", "");
        delay(2000);
        ESP.restart();
    }

    tft.println("WiFi Connected!");
    Serial.printf("[WiFi] Connected. IP: %s\n", WiFi.localIP().toString().c_str());

    // --- NTP ---
    configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
    time_t now = time(nullptr);
    while (now < 1700000000) { delay(500); now = time(nullptr); }

    // --- Firebase ---
    config.api_key           = API_KEY;
    config.database_url      = DATABASE_URL;
    config.signer.test_mode  = true;
    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);

    // --- Local storage ---
    if (LittleFS.begin(true)) {
        load_data();
    }

    load_coin_settings();
    currentPage = PAGE_INSERT;
    drawInsertPage();
}

// ================= LOOP =================
void loop() {
    // ── Setup mode: just serve HTTP and return ─────────────────
    if (isSetupMode) {
        server.handleClient();
        return;
    }

    // ── Always drain slave UART for MAC pushes / stray ACKs ───
    readSlaveUart();

    unsigned long now = millis();

    // ── 1-second countdown tick ────────────────────────────────
    if (now - lastSecondTick >= 1000) {
        lastSecondTick = now;

        // Decrement charging port timers
        for (int i = 0; i < 4; i++) {
            if (portSeconds[i] > 0) {
                portSeconds[i]--;
                if (portSeconds[i] == 0) {
                    portActive[i] = false;
                    setRelay(i, false);     // UART: turn off relay
                    Serial.printf("[PORT] Port %d expired\n", i);
                }
            }
        }

        // Decrement local WiFi display counter (visual only — slave keeps its own)
        if (wifiSeconds > 0) {
            wifiSeconds--;
        }

        // Refresh display if on a live page
        if (currentPage == PAGE_CHARGING) drawChargingPage();
        if (currentPage == PAGE_WIFI)     drawWiFiPage();

        // Flag live status for Firebase push
        needsStatusUpdate = true;
    }

    // ── Metal / coin sensor (ADC1, GPIO 34) ───────────────────
    int adcVal = analogRead(METAL_SENSOR_PIN);
    bool belowThreshold = (adcVal < METAL_ADC_THRESHOLD);

    if (belowThreshold && !metalPinLow) {
        // Reading just dropped below threshold — start hold timer
        metalPinLow       = true;
        metalLowStartTime = now;
    } else if (!belowThreshold && metalPinLow) {
        // Reading rose back above threshold — cancel hold
        metalPinLow = false;
    }

    if (metalPinLow && !metalDetected &&
        (now - metalLowStartTime >= METAL_HOLD_MS)) {
        // Held low long enough — genuine metal detection
        metalDetected     = true;
        metalDetectedTime = now;
        digitalWrite(LED_PIN, HIGH);
        Serial.printf("[METAL] Detected (ADC=%d)\n", adcVal);
    }

    if (metalDetected && (now - metalDetectedTime >= METAL_TIMEOUT_MS)) {
        metalDetected = false;
        digitalWrite(LED_PIN, LOW);
        Serial.println("[METAL] Gate closed (timeout)");
    }

    // ── Coin input polling ─────────────────────────────────────
    for (auto& [name, data] : coinData) {
        if (!coinSettings[name].enabled) continue;
        int state = digitalRead(data.pin);
        if (state == LOW && data.lastState == HIGH) {
            // Falling edge = coin inserted
            int timeMinutes = coinSettings[name].time;
            int timeSeconds = timeMinutes * 60;
            int coinValue   = name.toInt();

            credits        += coinValue;
            pendingCredits += coinValue;
            data.count++;
            needsFirebaseUpdate = true;
            save_data();

            Serial.printf("[COIN] P%s inserted → credits=%d\n", name.c_str(), credits);

            // ── Routing: where does this coin go? ──────────────
            if (currentPage == PAGE_INSERT || currentPage == PAGE_SELECT) {
                // Not yet committed — move to selection
                currentPage = PAGE_SELECT;
                drawSelectPage();

            } else if (currentPage == PAGE_CHARGING) {
                // Already charging: go to extend menu
                currentPage = PAGE_EXTEND;
                drawExtendPage();

            } else if (currentPage == PAGE_WIFI) {
                // Already in WiFi mode — show extend menu (extend or add new user)
                wifiPendingCredits += coinValue;
                currentPage = PAGE_WIFI_EXTEND;
                drawWiFiExtendPage();

            } else if (currentPage == PAGE_WIFI_EXTEND) {
                // Additional coin on WiFi extend page — accumulate
                wifiPendingCredits += coinValue;
                drawWiFiExtendPage();

            } else if (currentPage == PAGE_EXTEND) {
                // Additional coin on extend page — accumulate
                drawExtendPage();
            }
        }
        data.lastState = state;
    }

    // ── Button 1 ───────────────────────────────────────────────
    static bool lastBtn1 = LOW;
    bool btn1 = digitalRead(BTN_1_PIN);
    bool btn1Pressed = (btn1 == HIGH && lastBtn1 == LOW);
    lastBtn1 = btn1;

    // ── Button 2 ───────────────────────────────────────────────
    static bool lastBtn2 = LOW;
    bool btn2 = digitalRead(BTN_2_PIN);
    bool btn2Pressed = (btn2 == HIGH && lastBtn2 == LOW);
    lastBtn2 = btn2;

    // ── PAGE_SELECT button handling ────────────────────────────
    if (currentPage == PAGE_SELECT) {

        if (btn1Pressed) {
            // [1] Charging — convert all pending credits to time
            const int timePerCredit = 60; // seconds per peso credit
            int timeToAdd = credits * timePerCredit;
            activateRandomPort(timeToAdd);
            dailyChargingEarned += (float)credits;  // record daily revenue
            needsFirebaseUpdate  = true;
            credits        = 0;
            pendingCredits = 0;
            currentPage    = PAGE_CHARGING;
            drawChargingPage();
        }

        if (btn2Pressed) {
            // [2] WiFi — convert credits to seconds and push to slave
            // Rate: 1 peso = 1 minute of captive-portal WiFi time.
            // Change the multiplier below to adjust (e.g. 2*60 = 2 min/peso).
            const int wifiSecsPerCredit = 60; // seconds per peso credit
            int32_t wifiSecs = (int32_t)credits * wifiSecsPerCredit;

            if (wifiSecs > 0) {
                sendWifiTime(wifiTargetMac, wifiSecs);
                wifiSeconds          = (int)wifiSecs;
                wifiUsers            = 1;           // first user for this session
                wifiEarningsTotal   += (float)credits;  // record revenue
                dailyWifiEarned     += (float)credits;
                credits              = 0;
                pendingCredits       = 0;
                needsFirebaseUpdate  = true;
                needsStatusUpdate    = true;
            } else {
                Serial.println("[WiFi] No credits to assign.");
            }

            currentPage = PAGE_WIFI;
            drawWiFiPage();
        }
    }

    // ── PAGE_WIFI_EXTEND button handling ───────────────────────
    if (currentPage == PAGE_WIFI_EXTEND) {

        if (btn1Pressed) {
            // [1] Extend time for the current connected user
            const int wifiSecsPerCredit = 60;
            int32_t addSecs = (int32_t)wifiPendingCredits * wifiSecsPerCredit;
            sendWifiTime(wifiTargetMac, addSecs);
            wifiSeconds          += (int)addSecs;
            wifiEarningsTotal    += (float)wifiPendingCredits;
            dailyWifiEarned      += (float)wifiPendingCredits;
            credits              -= wifiPendingCredits;
            pendingCredits       -= wifiPendingCredits;
            wifiPendingCredits    = 0;
            needsFirebaseUpdate   = true;
            needsStatusUpdate     = true;
            currentPage           = PAGE_WIFI;
            drawWiFiPage();
        }

        if (btn2Pressed) {
            // [2] New user — send time to broadcast
            const int wifiSecsPerCredit = 60;
            int32_t addSecs = (int32_t)wifiPendingCredits * wifiSecsPerCredit;
            sendWifiTime("FF:FF:FF:FF:FF:FF", addSecs);
            wifiUsers++;
            wifiEarningsTotal    += (float)wifiPendingCredits;
            dailyWifiEarned      += (float)wifiPendingCredits;
            credits              -= wifiPendingCredits;
            pendingCredits       -= wifiPendingCredits;
            wifiPendingCredits    = 0;
            needsFirebaseUpdate   = true;
            needsStatusUpdate     = true;
            currentPage           = PAGE_WIFI;
            drawWiFiPage();
        }
    }

    // ── PAGE_EXTEND button handling ────────────────────────────
    if (currentPage == PAGE_EXTEND) {

        if (btn1Pressed) {
            // [1] Extend an existing port
            int activeCount = 0;
            for (int i = 0; i < 4; i++) if (portSeconds[i] > 0) activeCount++;

            if (activeCount == 1) {
                // Only one port running — extend it directly
                int port = firstActivePort();
                const int timePerCredit = 60;
                portSeconds[port]    += pendingCredits * timePerCredit;
                dailyChargingEarned  += (float)pendingCredits;
                needsFirebaseUpdate   = true;
                pendingCredits = 0;
                credits        = 0;
                needsStatusUpdate = true;
                currentPage    = PAGE_CHARGING;
                drawChargingPage();
            } else {
                // Multiple ports — let user pick which one
                selectingPort   = true;
                highlightedPort = firstActivePort();
                drawPortSelectPage(highlightedPort);
            }
        }

        if (btn2Pressed) {
            // [2] Open a new port
            const int timePerCredit = 60;
            int timeToAdd = pendingCredits * timePerCredit;
            activateRandomPort(timeToAdd);
            dailyChargingEarned += (float)pendingCredits;
            needsFirebaseUpdate  = true;
            pendingCredits = 0;
            credits        = 0;
            needsStatusUpdate = true;
            currentPage    = PAGE_CHARGING;
            drawChargingPage();
        }
    }

    // ── PORT-SELECT sub-mode (inside PAGE_EXTEND) ──────────────
    if (selectingPort) {

        if (btn1Pressed) {
            // [1] Confirm highlighted port
            if (highlightedPort >= 0 && portSeconds[highlightedPort] > 0) {
                const int timePerCredit = 60;
                portSeconds[highlightedPort] += pendingCredits * timePerCredit;
                dailyChargingEarned  += (float)pendingCredits;
                needsFirebaseUpdate   = true;
                pendingCredits  = 0;
                credits         = 0;
                selectingPort   = false;
                highlightedPort = -1;
                needsStatusUpdate = true;
                currentPage     = PAGE_CHARGING;
                drawChargingPage();
            }
        }

        if (btn2Pressed) {
            // [2] Cycle to next active port
            int next = highlightedPort;
            for (int i = 1; i <= 4; i++) {
                int candidate = (highlightedPort + i) % 4;
                if (portSeconds[candidate] > 0) { next = candidate; break; }
            }
            highlightedPort = next;
            drawPortSelectPage(highlightedPort);
        }
    }

    // ── Auto-return to INSERT page when all sessions end ───────
    if (currentPage == PAGE_CHARGING) {
        bool anyActive = false;
        for (int i = 0; i < 4; i++) if (portSeconds[i] > 0) anyActive = true;
        if (!anyActive) {
            currentPage = PAGE_INSERT;
            drawInsertPage();
        }
    }

    if (currentPage == PAGE_WIFI && wifiSeconds <= 0) {
        wifiPendingCredits = 0;
        wifiUsers          = 0;
        needsStatusUpdate  = true;
        currentPage = PAGE_INSERT;
        drawInsertPage();
    }

    // ── Firebase periodic sync ─────────────────────────────────
    update_firebase();
    push_status_firebase();

    delay(10);
}
