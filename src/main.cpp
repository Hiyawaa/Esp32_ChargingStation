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
// Wire: Master TX2 (GPIO 17) → Slave RX (GPIO 4)
//       Master RX2 (GPIO 16) ← Slave TX (GPIO 5)
//       Common GND
#define SLAVE_UART_TX   17
#define SLAVE_UART_RX   16
#define SLAVE_UART_BAUD 115200

// ──────────────────────────────────────────────────────────────
// UART COMMAND HELPERS
//
// Relay commands:
//   "RELAY:<port>:ON\n"   → slave turns relay ON  (GPIO LOW,  active-low)
//   "RELAY:<port>:OFF\n"  → slave turns relay OFF (GPIO HIGH)
//   "RELAY:ALL:OFF\n"     → slave turns ALL relays OFF
//
// WiFi credential forwarding (setup mode only):
//   "WIFI:<ssid>|<pass>\n" → slave saves to NVS and reboots to connect
//   The '|' delimiter is used because it rarely appears in SSIDs or passwords.
// ──────────────────────────────────────────────────────────────
void setRelay(int port, bool on) {
    Serial2.printf("RELAY:%d:%s\n", port, on ? "ON" : "OFF");
    delay(10);
}

void setAllRelaysOff() {
    Serial2.println("RELAY:ALL:OFF");
    delay(10);
}

void forwardWiFiToSlave(const String& ssid, const String& pass) {
    // Format: "WIFI:<ssid>|<pass>\n"
    Serial2.printf("WIFI:%s|%s\n", ssid.c_str(), pass.c_str());
    Serial.printf("[UART→SLAVE] Sent WiFi creds  SSID='%s'  PASS='%s'\n",
                  ssid.c_str(), pass.c_str());

    // ── Wait for slave ACK ────────────────────────────────────
    // Slave replies "ACK:WIFI:SAVED\n" after writing to NVS,
    // or an ERR:* string if something went wrong.
    // We allow up to 3 000 ms before declaring a timeout.
    const unsigned long ACK_TIMEOUT_MS = 3000;
    unsigned long       deadline       = millis() + ACK_TIMEOUT_MS;
    String              reply          = "";

    while (millis() < deadline) {
        while (Serial2.available()) {
            char c = (char)Serial2.read();
            if (c == '\n') {
                reply.trim();
                if (reply.length() > 0) goto ack_done;
                reply = "";          // skip blank lines
            } else if (c != '\r') {
                reply += c;
            }
        }
        delay(10);
    }

ack_done:
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

    // Give the slave's NVS write + reboot a moment to begin cleanly.
    delay(500);
}

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
    PAGE_EXTEND
};

Page currentPage = PAGE_INSERT;

int  credits           = 0;
int  pendingCredits    = 0;
int  unassignedSeconds = 0;
int  portSeconds[4]    = {0, 0, 0, 0};
int  wifiSeconds       = 0;
bool portActive[4]     = {false, false, false, false};
int  lastActivatedPort = 0;
int  wifiUsers         = 3;

bool          showText       = true;
unsigned long lastBlink      = 0;
unsigned long lastSecondTick = 0;

// ── EXTEND / PORT-SELECT STATE ────────────────────────────────
bool selectingPort   = false;   // true while user is picking a port to extend
int  highlightedPort = -1;      // which port is currently highlighted in picker

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

bool          needsFirebaseUpdate = false;
unsigned long lastFirebaseUpdate  = 0;
const unsigned long firebaseInterval = 3600000UL;

bool          metalDetected      = false;
unsigned long metalDetectedTime  = 0;
unsigned long metalLowStartTime  = 0;    // when reading first dropped below threshold
bool          metalPinLow        = false; // true while reading is actively below threshold

// ESP32 ADC: 0–4095 maps to 0–3.3 V
// Metal present  → ~2.4 V → ADC ~2979
// Metal absent   → ~3.3 V → ADC ~4095
// Trigger threshold set at ~2.8 V (ADC ~3482) — comfortably between the two levels.
// Lower this value if you want more sensitivity; raise it if you get false triggers.
const int     METAL_ADC_THRESHOLD = 3000;

// Pin must stay below threshold for this long before gate opens (noise filter)
const unsigned long METAL_HOLD_MS    = 30;
// Gate auto-closes if no coin arrives within this window after metal is released
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

// Returns the index of the first port with remaining time, or -1 if none
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
        // All ports busy — add time to the last activated port
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
    tft.print("Credits: P");
    tft.println(credits);
    tft.setCursor(20, 60);
    tft.setTextColor(OK);
    tft.println("[1] Charging");
    tft.setCursor(20, 80);
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

void drawWiFiPage() {
    tft.fillScreen(BG);
    drawHeader();
    tft.setCursor(10, 30);
    tft.setTextColor(HL);
    tft.print("Time: ");
    tft.println(formatTime(wifiSeconds));
    tft.setCursor(10, 60);
    tft.setTextColor(OK);
    tft.print("Devices: ");
    tft.println(wifiUsers);
    tft.setCursor(10, 80);
    tft.setTextColor(TXT);
    tft.println("SSID: VENDO_WIFI");
    tft.drawRect(0, 0, 160, 128, TXT);
}

// ──────────────────────────────────────────────────────────────
// drawExtendPage()
//
// Shows pending coin value AND all currently active ports with
// their remaining time so the user knows which ports exist
// before deciding to extend or open a new one.
// ──────────────────────────────────────────────────────────────
void drawExtendPage() {
    tft.fillScreen(BG);
    drawHeader();

    // Pending credits line
    tft.setCursor(5, 22);
    tft.setTextColor(HL);
    tft.setTextSize(1);
    tft.print("Added: P");
    tft.println(pendingCredits);

    // List every active port with remaining time
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

    // Bottom action hints
    tft.setCursor(5, 100);
    tft.setTextColor(OK);
    tft.println("[1] Extend Time");
    tft.setCursor(5, 112);
    tft.setTextColor(HL);
    tft.println("[2] New Port");
    tft.drawRect(0, 0, 160, 128, TXT);
}

// ──────────────────────────────────────────────────────────────
// drawPortSelectPage()
//
// Shown when the user pressed [1] on the extend page.
// Scrolls a highlight marker through active ports so the user
// can pick exactly which port receives the extra time.
// ──────────────────────────────────────────────────────────────
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

    float totalCharging = 0;
    FirebaseJson json;
    for (auto const& [name, data] : coinData) {
        json.set("settings/coins/" + name + "/count", data.count);
        totalCharging += name.toInt() * data.count;
    }

    json.set("total_earnings/charging", totalCharging);
    String today = getToday();
    json.set("logs/" + today + "/charging", totalCharging);

    if (Firebase.RTDB.updateNode(&fbdo, "/", &json)) {
        needsFirebaseUpdate = false;
    }
    lastFirebaseUpdate = now;
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
    // (No relay pinMode — relays are managed entirely by the slave ESP32)
    pinMode(LED_PIN, OUTPUT);
    pinMode(METAL_SENSOR_PIN, INPUT);   // GPIO 34: ADC1, input-only, no pull resistors
    pinMode(BTN_1_PIN, INPUT_PULLDOWN);
    pinMode(BTN_2_PIN, INPUT_PULLDOWN);
    for (auto const& [name, data] : coinData) {
        pinMode(data.pin, INPUT_PULLUP);
    }

    // --- UART2: open channel to slave BEFORE smart-setup check ---
    // This ensures forwardWiFiToSlave() works even when we enter setup mode.
    Serial2.begin(SLAVE_UART_BAUD, SERIAL_8N1, SLAVE_UART_RX, SLAVE_UART_TX);
    delay(200);
    setAllRelaysOff(); // slave starts with all relays OFF regardless of mode

    // --- Smart Setup ---
    preferences.begin("wifi_creds", false);
    String saved_ssid = preferences.getString("ssid", "");
    String saved_pass = preferences.getString("pass", "");

    if (saved_ssid == "") {
        // ── SETUP MODE ──────────────────────────────────────────
        // No credentials saved yet. Start AP so the Flutter app
        // can POST the home WiFi SSID/password to /setup.
        // handleSetup() will also forward them to the slave via UART
        // so both boards connect to the same network automatically.
        isSetupMode = true;
        currentPage = PAGE_SETUP;

        WiFi.mode(WIFI_AP);
        WiFi.softAP("ESP32_VENDO", "12345678");

        server.on("/setup", HTTP_POST, handleSetup);
        server.begin();

        drawSetupPage();
        Serial.println("[SETUP] AP active. Waiting for Flutter app...");
        Serial.printf("[SETUP] AP IP: %s\n", WiFi.softAPIP().toString().c_str());
        return; // skip normal init
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
        // Credentials stale — wipe and reboot into setup mode
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

// ================= FIREBASE REMOTE COMMANDS =================
void check_wifi_change_command() {
    if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck < 10000) return;
    lastCheck = millis();

    if (Firebase.RTDB.getJSON(&fbdo, "/commands/wifi_change")) {
        DynamicJsonDocument doc(256);
        if (deserializeJson(doc, fbdo.jsonString()) != DeserializationError::Ok) return;
        if (!doc["pending"].as<bool>()) return;

        String newSsid = doc["ssid"].as<String>();
        String newPass = doc["pass"].as<String>();
        if (newSsid.isEmpty()) return;

        Serial.printf("[WiFi Change] New SSID: %s\n", newSsid.c_str());

        FirebaseJson clearJson; clearJson.set("pending", false);
        Firebase.RTDB.updateNode(&fbdo, "/commands/wifi_change", &clearJson);

        tft.fillScreen(BG); drawHeader();
        tft.setCursor(10, 35); tft.setTextColor(HL); tft.println("WiFi Change");
        tft.setCursor(10, 52); tft.setTextColor(TXT); tft.println("Saving & rebooting");
        tft.setCursor(10, 68); tft.setTextColor(OK); tft.println(newSsid);
        tft.drawRect(0, 0, 160, 128, TXT);
        delay(800);

        preferences.putString("ssid", newSsid);
        preferences.putString("pass", newPass);
        forwardWiFiToSlave(newSsid, newPass);
        ESP.restart();
    }
}

void check_wifi_show_command() {
    if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck < 10000) return;
    lastCheck = millis();

    if (Firebase.RTDB.getJSON(&fbdo, "/commands/wifi_show")) {
        DynamicJsonDocument doc(128);
        if (deserializeJson(doc, fbdo.jsonString()) != DeserializationError::Ok) return;
        if (!doc["pending"].as<bool>()) return;

        // 1. Clear the command so it only fires once
        FirebaseJson clearJson; clearJson.set("pending", false);
        Firebase.RTDB.updateNode(&fbdo, "/commands/wifi_show", &clearJson);

        // 2. Read saved SSID from NVS (password intentionally hidden for security)
        String savedSsid = preferences.getString("ssid", "(not set)");

        // 3. Display on TFT
        tft.fillScreen(BG);
        drawHeader();
        tft.setCursor(10, 28); tft.setTextColor(HL);  tft.setTextSize(1);
        tft.println("WiFi Credentials");
        tft.setCursor(10, 45); tft.setTextColor(TXT);
        tft.println("Connected Network:");
        tft.setCursor(10, 58); tft.setTextColor(OK);  tft.setTextSize(1);
        tft.println(savedSsid);
        tft.setCursor(10, 76); tft.setTextColor(TXT);
        tft.println("Password: ********");
        tft.setCursor(10, 96); tft.setTextColor(ST77XX_CYAN);
        tft.println("(Saved in memory)");
        tft.drawRect(0, 0, 160, 128, HL);

        // 4. Hold for 5 seconds then redraw the normal page
        delay(5000);
        switch (currentPage) {
            case PAGE_INSERT:   drawInsertPage();   break;
            case PAGE_SELECT:   drawSelectPage();   break;
            case PAGE_CHARGING: drawChargingPage(); break;
            case PAGE_WIFI:     drawWiFiPage();     break;
            case PAGE_EXTEND:
                if (selectingPort) drawPortSelectPage(highlightedPort);
                else               drawExtendPage();
                break;
            default: drawInsertPage(); break;
        }
    }
}

// ================= LOOP =================
void loop() {
    // Setup mode: only serve the config web page
    if (isSetupMode) {
        server.handleClient();
        return;
    }

    // ── 1. METAL SENSOR (analog) ─────────────────────────────────
    // digitalRead() cannot detect the 3.3→2.4 V drop from this sensor
    // because 2.4 V is still above the ESP32 HIGH threshold (~1.8 V).
    // analogRead() gives us the actual voltage level so we can set our
    // own threshold between the idle voltage and the metal-present voltage.
    {
        int metalADC = analogRead(METAL_SENSOR_PIN);
        bool metalBelow = (metalADC < METAL_ADC_THRESHOLD);

        if (metalBelow) {
            if (!metalPinLow) {
                // Reading just dropped below threshold — start hold timer
                metalPinLow       = true;
                metalLowStartTime = millis();
                Serial.printf("[METAL] Below threshold (ADC=%d) — starting hold timer\n",
                              metalADC);
            } else if (!metalDetected &&
                       (millis() - metalLowStartTime >= METAL_HOLD_MS)) {
                // Held below threshold long enough — open the gate
                metalDetected     = true;
                metalDetectedTime = millis();
                Serial.printf("[METAL] Confirmed (ADC=%d) — coin gate OPEN\n", metalADC);
            }
        } else {
            // Reading back above threshold — metal has passed
            if (metalPinLow) {
                Serial.printf("[METAL] Released (ADC=%d)\n", metalADC);
            }
            metalPinLow = false;
            // Do NOT clear metalDetected — coin IR must consume it or timeout expires it
        }

        // Auto-expire: gate open too long with no coin following
        if (metalDetected && (millis() - metalDetectedTime > METAL_TIMEOUT_MS)) {
            metalDetected = false;
            metalPinLow   = false;
            Serial.println("[METAL] Gate expired — CLOSED (timeout)");
        }

        // Continuous debug — comment out after tuning is done
        static unsigned long lastADCPrint = 0;
        if (millis() - lastADCPrint > 500) {
            lastADCPrint = millis();
            Serial.printf("[METAL DEBUG] ADC=%d  (%.2fV)  gate=%s\n",
                          metalADC,
                          metalADC * 3.3f / 4095.0f,
                          metalDetected ? "OPEN" : "closed");
        }
    }

    // ── 2. COIN SENSOR POLLING ───────────────────────────────────
    // Require the IR pin to stay LOW for 10 ms to reject noise glitches.
    for (auto& [name, data] : coinData) {
        int currentState = digitalRead(data.pin);

        if (currentState == LOW && data.lastState == HIGH) {
            delay(10); // hold-time check — must still be LOW after 10 ms
            if (digitalRead(data.pin) == LOW) {

                if (metalDetected && coinSettings[name].enabled) {
                    Serial.printf("[COIN] Accepted P%s (metal age: %lu ms)\n",
                                  name.c_str(), millis() - metalDetectedTime);

                    metalDetected = false; // one metal detection = one coin only
                    metalPinLow   = false;
                    Serial.println("[METAL] Gate consumed — CLOSED");

                    data.count++;
                    save_data();
                    needsFirebaseUpdate = true;

                    int coinValue    = name.toInt();
                    int addedSeconds = coinSettings[name].time * 60;

                    if (currentPage == PAGE_INSERT) {
                        credits           += coinValue;
                        unassignedSeconds += addedSeconds;
                        currentPage = PAGE_SELECT;
                        drawSelectPage();
                    }
                    else if (currentPage == PAGE_SELECT) {
                        credits           += coinValue;
                        unassignedSeconds += addedSeconds;
                        drawSelectPage();
                    }
                    else if (currentPage == PAGE_CHARGING ||
                             currentPage == PAGE_WIFI     ||
                             currentPage == PAGE_EXTEND) {
                        pendingCredits    += coinValue;
                        unassignedSeconds += addedSeconds;

                        // A new coin interrupts any in-progress port selection
                        // and brings the user back to the extend choice screen.
                        selectingPort   = false;
                        highlightedPort = -1;

                        currentPage = PAGE_EXTEND;
                        drawExtendPage();
                    }

                } else if (!metalDetected) {
                    Serial.printf("[COIN] REJECTED P%s — gate closed (no metal)\n",
                                  name.c_str());
                } else if (!coinSettings[name].enabled) {
                    Serial.printf("[COIN] REJECTED P%s — disabled in settings\n",
                                  name.c_str());
                }
            }
        }
        data.lastState = currentState;
    }

    // ── 3. SCREEN & BUTTON STATE MACHINE ─────────────────────────

    // PAGE: INSERT — blink "INSERT COIN"
    if (currentPage == PAGE_INSERT) {
        if (millis() - lastBlink > 500) {
            lastBlink = millis();
            showText  = !showText;
            tft.fillRect(20, 100, 120, 20, BG);
            if (showText) {
                tft.setCursor(25, 100);
                tft.setTextColor(HL);
                tft.print("INSERT COIN");
            }
        }
    }

    // PAGE: SELECT — choose Charging or WiFi
    else if (currentPage == PAGE_SELECT) {
        if (digitalRead(BTN_1_PIN) == HIGH) {
            activateRandomPort(unassignedSeconds);
            unassignedSeconds = 0;

            currentPage    = PAGE_CHARGING;
            lastSecondTick = millis();
            drawChargingPage();
            delay(300);
        }
        else if (digitalRead(BTN_2_PIN) == HIGH) {
            wifiSeconds      += unassignedSeconds;
            unassignedSeconds = 0;

            currentPage    = PAGE_WIFI;
            lastSecondTick = millis();
            drawWiFiPage();
            delay(300);
        }
    }

    // PAGE: CHARGING — per-port countdown
    else if (currentPage == PAGE_CHARGING) {
        if (millis() - lastSecondTick >= 1000) {
            lastSecondTick = millis();
            bool anyPortActive = false;

            for (int i = 0; i < 4; i++) {
                if (portSeconds[i] > 0) {
                    portSeconds[i]--;
                    anyPortActive = true;

                    if (portSeconds[i] <= 0) {
                        portActive[i] = false;
                        setRelay(i, false);         // ← UART: cut this port's relay
                    }
                }
            }

            if (anyPortActive) {
                tft.fillRect(2, 22, 156, 100, BG);
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
            } else {
                credits = 0;
                deactivateAllPorts();               // ← UART: all relays OFF
                currentPage = PAGE_INSERT;
                drawInsertPage();
            }
        }
    }

    // PAGE: WIFI — countdown
    else if (currentPage == PAGE_WIFI) {
        if (millis() - lastSecondTick >= 1000) {
            lastSecondTick = millis();
            if (wifiSeconds > 0) {
                wifiSeconds--;
                tft.fillRect(40, 30, 110, 20, BG);
                tft.setCursor(10, 30);
                tft.setTextColor(HL);
                tft.print("Time: ");
                tft.println(formatTime(wifiSeconds));
            } else {
                credits = 0;
                currentPage = PAGE_INSERT;
                drawInsertPage();
            }
        }
    }

    // PAGE: EXTEND — coin inserted mid-session
    // ──────────────────────────────────────────────────────────────
    // Two sub-states controlled by the `selectingPort` flag:
    //
    //  selectingPort == false  →  Main extend screen
    //    [BTN_1]  Enter port-select mode  (extend time on a specific port)
    //    [BTN_2]  Activate a new free port; if all ports full → enter port-select
    //
    //  selectingPort == true   →  Port picker screen
    //    [BTN_2]  Cycle the highlight to the next active port
    //    [BTN_1]  Confirm: add unassignedSeconds to the highlighted port
    // ──────────────────────────────────────────────────────────────
    else if (currentPage == PAGE_EXTEND) {

        // ── SUB-STATE: port picker ────────────────────────────────
        if (selectingPort) {

            // BTN_2 → move highlight to next active port
            if (digitalRead(BTN_2_PIN) == HIGH) {
                // Search for the next active port after the current highlight
                int next = -1;
                for (int i = 1; i <= 4; i++) {
                    int candidate = (highlightedPort + i) % 4;
                    if (portSeconds[candidate] > 0) {
                        next = candidate;
                        break;
                    }
                }
                // Only redraw if we actually moved to a different port
                if (next != -1 && next != highlightedPort) {
                    highlightedPort = next;
                    drawPortSelectPage(highlightedPort);
                }
                delay(300);
            }

            // BTN_1 → confirm selection, add time, return to charging
            else if (digitalRead(BTN_1_PIN) == HIGH) {
                portSeconds[highlightedPort] += unassignedSeconds;
                credits          += pendingCredits;
                unassignedSeconds = 0;
                pendingCredits    = 0;
                selectingPort     = false;
                highlightedPort   = -1;

                currentPage    = PAGE_CHARGING;
                lastSecondTick = millis();
                drawChargingPage();
                delay(300);
            }
        }

        // ── SUB-STATE: extend / new-port choice ──────────────────
        else {

            // BTN_1 → enter port-select mode so user picks which port to extend
            if (digitalRead(BTN_1_PIN) == HIGH) {
                int fp = firstActivePort();
                if (fp != -1) {
                    selectingPort   = true;
                    highlightedPort = fp;
                    drawPortSelectPage(highlightedPort);
                }
                delay(300);
            }

            // BTN_2 → try to open a brand-new port
            else if (digitalRead(BTN_2_PIN) == HIGH) {
                // Check how many ports are truly free
                int available = 0;
                for (int i = 0; i < 4; i++) {
                    if (!portActive[i] && portSeconds[i] <= 0) available++;
                }

                if (available > 0) {
                    // At least one free port → activate it immediately
                    activateRandomPort(unassignedSeconds);
                    credits          += pendingCredits;
                    unassignedSeconds = 0;
                    pendingCredits    = 0;

                    currentPage    = PAGE_CHARGING;
                    lastSecondTick = millis();
                    drawChargingPage();
                } else {
                    // All 4 ports busy → fall through to port-select for extension
                    int fp = firstActivePort();
                    if (fp != -1) {
                        selectingPort   = true;
                        highlightedPort = fp;
                        drawPortSelectPage(highlightedPort);
                    }
                }
                delay(300);
            }
        }
    }

    // ── 4. BACKGROUND TASKS ──────────────────────────────────────
    update_firebase();
    check_wifi_change_command();
    check_wifi_show_command();
}