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
const int METAL_SENSOR_PIN = 27;
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
    Serial.printf("[UART] Forwarded WiFi creds to slave: SSID=%s\n", ssid.c_str());
    // Give the slave enough time to receive, write to NVS, and begin its reboot.
    delay(1500);
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

int  lastMetalState = HIGH;
bool metalDetected  = false;

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

void drawExtendPage() {
    tft.fillScreen(BG);
    drawHeader();
    tft.setCursor(10, 40);
    tft.setTextColor(HL);
    tft.print("Added: P");
    tft.println(pendingCredits);
    tft.setCursor(10, 70);
    tft.setTextColor(OK);
    tft.println("[1] Extend Time");
    tft.setCursor(10, 90);
    tft.setTextColor(TXT);
    tft.println("[2] New Port");
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
    pinMode(METAL_SENSOR_PIN, INPUT_PULLUP);
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
void check_wifi_reset_command() {
    if (WiFi.status() != WL_CONNECTED || !Firebase.ready()) return;
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck < 10000) return;
    lastCheck = millis();

    if (Firebase.RTDB.getJSON(&fbdo, "/commands/wifi_reset")) {
        DynamicJsonDocument doc(128);
        if (deserializeJson(doc, fbdo.jsonString()) != DeserializationError::Ok) return;
        if (!doc["pending"].as<bool>()) return;

        Serial.println("[WiFi Reset] Wiping credentials and rebooting to Setup Mode...");

        // 1. Clear the command node so it doesn't re-trigger
        FirebaseJson clearJson; clearJson.set("pending", false);
        Firebase.RTDB.updateNode(&fbdo, "/commands/wifi_reset", &clearJson);

        // 2. Show feedback on TFT
        tft.fillScreen(BG); drawHeader();
        tft.setCursor(10, 35); tft.setTextColor(OFF);  tft.println("WiFi Reset!");
        tft.setCursor(10, 52); tft.setTextColor(TXT);  tft.println("Clearing credentials");
        tft.setCursor(10, 68); tft.setTextColor(HL);   tft.println("Reboot -> Setup Mode");
        tft.drawRect(0, 0, 160, 128, TXT);
        delay(800);

        // 3. Wipe master NVS
        preferences.putString("ssid", "");
        preferences.putString("pass", "");

        // 4. Tell slave to wipe its NVS too
        //    Slave listens for "WIFI_RESET\n" and clears its own Preferences
        Serial2.println("WIFI_RESET");
        delay(1000);

        // 5. Reboot master — empty ssid → setup() enters AP / Setup Mode
        ESP.restart();
    }
}
// ================= LOOP =================
void loop() {
    // Setup mode: only serve the config web page
    if (isSetupMode) {
        server.handleClient();
        return;
    }

    // ── 1. METAL SENSOR ──────────────────────────────────────────
    int metalState = digitalRead(METAL_SENSOR_PIN);
    if (metalState != lastMetalState) {
        if (metalState == LOW) metalDetected = true;
        lastMetalState = metalState;
        delay(50);
    }

    // ── 2. COIN SENSOR POLLING ───────────────────────────────────
    for (auto& [name, data] : coinData) {
        int currentState = digitalRead(data.pin);

        if (currentState == LOW && data.lastState == HIGH) {
            delay(5);
            if (digitalRead(data.pin) == LOW) {

                if (metalDetected && coinSettings[name].enabled) {
                    data.count++;
                    save_data();
                    needsFirebaseUpdate = true;

                    int coinValue     = name.toInt();
                    int addedSeconds  = coinSettings[name].time * 60;

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
                        currentPage = PAGE_EXTEND;
                        drawExtendPage();
                    }

                    metalDetected = false;
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
    else if (currentPage == PAGE_EXTEND) {
        if (digitalRead(BTN_1_PIN) == HIGH) {
            portSeconds[lastActivatedPort] += unassignedSeconds;
            credits          += pendingCredits;
            unassignedSeconds = 0;
            pendingCredits    = 0;

            currentPage    = PAGE_CHARGING;
            lastSecondTick = millis();
            drawChargingPage();
            delay(300);
        }
        else if (digitalRead(BTN_2_PIN) == HIGH) {
            activateRandomPort(unassignedSeconds);
            credits          += pendingCredits;
            unassignedSeconds = 0;
            pendingCredits    = 0;

            currentPage    = PAGE_CHARGING;
            lastSecondTick = millis();
            drawChargingPage();
            delay(300);
        }
    }

    // ── 4. BACKGROUND TASKS ──────────────────────────────────────
    update_firebase();
    check_wifi_change_command();
}