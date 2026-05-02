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

// NEW INCLUDES FOR SMART SETUP
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
const int LED_PIN = 25;   

const int BTN_1_PIN = 22; 
const int BTN_2_PIN = 23; 
const int RELAY_PINS[4] = {12, 14, 16, 17}; 

struct CoinPin {
    int pin;
    int lastState;
    long count;
};

std::map<String, CoinPin> coinData = {
    {"1",  {5,  HIGH, 0}}, 
    {"5",  {18, HIGH, 0}},
    {"10", {19, HIGH, 0}},
    {"20", {21, HIGH, 0}}
};

// ================= UI STATES & DATA =================
enum Page {
  PAGE_SETUP, // New Page for when WiFi is missing
  PAGE_INSERT,
  PAGE_SELECT,
  PAGE_CHARGING,
  PAGE_WIFI,
  PAGE_EXTEND
};

Page currentPage = PAGE_INSERT;

int credits = 0;
int pendingCredits = 0;
int unassignedSeconds = 0; 
int portSeconds[4] = {0, 0, 0, 0}; 
int wifiSeconds = 0;
bool portActive[4] = {false, false, false, false}; 
int lastActivatedPort = 0; 
int wifiUsers = 3;

bool showText = true;
unsigned long lastBlink = 0;
unsigned long lastSecondTick = 0;

// ================= COLORS =================
#define BG ST77XX_BLACK
#define TXT ST77XX_WHITE
#define HL ST77XX_YELLOW
#define OK ST77XX_GREEN
#define OFF ST77XX_RED
#define HEADER ST77XX_BLUE

// ================= FIREBASE & WIFI SETTINGS =================
const char* API_KEY = "AIzaSyDGV3vPuxVRkHSBDESwc7nCud2Zz4jDjNM";
const char* DATABASE_URL = "esp32padin-default-rtdb.asia-southeast1.firebasedatabase.app";
const char* FILENAME = "/data.json";

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
bool needsFirebaseUpdate = false; 
unsigned long lastFirebaseUpdate = 0;
const unsigned long firebaseInterval = 3600000UL; 

int lastMetalState = HIGH;
bool metalDetected = false;

struct CoinSetting {
    bool enabled;
    int time; 
};

std::map<String, CoinSetting> coinSettings = {
    {"1",  {true, 5}},
    {"5",  {true, 15}},
    {"10", {true, 30}},
    {"20", {true, 60}}
};

// ================= SMART SETUP GLOBALS =================
Preferences preferences;
WebServer server(80);
bool isSetupMode = false;

// Handles the HTTP request from your Flutter App
void handleSetup() {
    if (server.hasArg("ssid") && server.hasArg("pass")) {
        String new_ssid = server.arg("ssid");
        String new_pass = server.arg("pass");
        
        // Save to internal memory
        preferences.putString("ssid", new_ssid);
        preferences.putString("pass", new_pass);
        
        server.send(200, "application/json", "{\"success\":true, \"message\":\"Credentials Saved! Rebooting...\"}");
        
        // Wait a second for the response to send, then reboot to apply
        delay(1000);
        ESP.restart();
    } else {
        server.send(400, "application/json", "{\"success\":false, \"message\":\"Missing parameters\"}");
    }
}

// ================= HELPER FUNCTIONS =================
int activateRandomPort(int timeToAdd) {
    int availablePorts[4];
    int availableCount = 0;
    
    for(int i = 0; i < 4; i++) {
        if (!portActive[i] && portSeconds[i] <= 0) {
            availablePorts[availableCount] = i;
            availableCount++;
        }
    }
    
    if (availableCount == 0) {
        portSeconds[lastActivatedPort] += timeToAdd;
        return lastActivatedPort;
    }
    
    int randomIndex = random(0, availableCount);
    int selectedPort = availablePorts[randomIndex];
    
    portActive[selectedPort] = true;
    portSeconds[selectedPort] = timeToAdd;
    digitalWrite(RELAY_PINS[selectedPort], LOW); 
    
    lastActivatedPort = selectedPort; 
    return selectedPort;
}

void deactivateAllPorts() {
    for(int i = 0; i < 4; i++) {
        portActive[i] = false;
        portSeconds[i] = 0;
        digitalWrite(RELAY_PINS[i], HIGH); 
    }
}

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

// ================= DISPLAY DRAWING =================
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
  tft.setCursor(10, 40);
  tft.setTextColor(HL);
  tft.println("SETUP MODE");
  tft.setCursor(10, 70);
  tft.setTextColor(TXT);
  tft.println("Connect App to:");
  tft.setCursor(10, 90);
  tft.setTextColor(OK);
  tft.println("ESP32_VENDO");
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
  for(int i = 0; i < 4; i++) {
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
  tft.println("Add Credit");
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

  SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(3);
  tft.fillScreen(BG);
  tft.setCursor(10, 50);
  tft.setTextColor(TXT);
  tft.setTextSize(1);
  tft.println("Booting up...");

  pinMode(LED_PIN, OUTPUT);
  pinMode(METAL_SENSOR_PIN, INPUT_PULLUP);
  
  for(int i = 0; i < 4; i++) {
      pinMode(RELAY_PINS[i], OUTPUT);
      digitalWrite(RELAY_PINS[i], HIGH);
  }
  
  pinMode(BTN_1_PIN, INPUT_PULLDOWN);
  pinMode(BTN_2_PIN, INPUT_PULLDOWN);
  for (auto const& [name, data] : coinData) pinMode(data.pin, INPUT_PULLUP);

  // ---------------------------------------------------------
  // SMART SETUP LOGIC
  // ---------------------------------------------------------
  preferences.begin("wifi_creds", false);
  String saved_ssid = preferences.getString("ssid", "");
  String saved_pass = preferences.getString("pass", "");

  if (saved_ssid == "") {
      // SCENARIO 1: No WiFi saved. Start Setup Mode.
      isSetupMode = true;
      currentPage = PAGE_SETUP;
      
      WiFi.mode(WIFI_AP);
      WiFi.softAP("ESP32_VENDO", "12345678"); 
      
      server.on("/setup", HTTP_POST, handleSetup);
      server.begin();
      
      drawSetupPage();
      Serial.println("Setup Mode active. Waiting for Flutter app...");
      
      return; // Stop running the rest of setup()
  } 
  else {
      // SCENARIO 2: WiFi is saved. Connect to Home Internet.
      tft.println("Connecting WiFi...");
      WiFi.mode(WIFI_STA);
      WiFi.begin(saved_ssid.c_str(), saved_pass.c_str());
      
      // Give it 15 seconds to connect
      int retries = 0;
      while (WiFi.status() != WL_CONNECTED && retries < 30) { 
          delay(500); 
          retries++;
      }

      // If home WiFi changed or password is wrong, wipe memory and reboot to Setup Mode
      if (WiFi.status() != WL_CONNECTED) {
          tft.println("WiFi Failed!");
          tft.println("Resetting...");
          preferences.putString("ssid", ""); // Clear memory
          delay(2000);
          ESP.restart(); 
      }
      
      tft.println("WiFi Connected!");
  }
  // ---------------------------------------------------------
  
  configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  time_t now = time(nullptr);
  while (now < 1700000000) { delay(500); now = time(nullptr); }

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  config.signer.test_mode = true;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  if (LittleFS.begin(true)) {
      load_data();
  }
  load_coin_settings();

  currentPage = PAGE_INSERT;
  drawInsertPage(); 
}

// ================= LOOP =================
void loop() {
    // If we are in Setup Mode, DO NOT run the vending logic. Just listen for the app.
    if (isSetupMode) {
        server.handleClient();
        return; 
    }

    // --- 1. HARDWARE COIN SENSOR POLLING ---
    int metalState = digitalRead(METAL_SENSOR_PIN);
    if (metalState != lastMetalState) {
        if (metalState == LOW) metalDetected = true;
        lastMetalState = metalState;
        delay(50); 
    }

    for (auto& [name, data] : coinData) {
        int currentState = digitalRead(data.pin);

        if (currentState == LOW && data.lastState == HIGH) {
            delay(5); 
            if (digitalRead(data.pin) == LOW) { 
                
                if (metalDetected && coinSettings[name].enabled) {
                    data.count++;
                    save_data();
                    needsFirebaseUpdate = true;
                    
                    int coinValue = name.toInt();
                    int addedSeconds = coinSettings[name].time * 60;

                    if (currentPage == PAGE_INSERT) {
                        credits += coinValue;
                        unassignedSeconds += addedSeconds;
                        currentPage = PAGE_SELECT;
                        drawSelectPage();
                    } 
                    else if (currentPage == PAGE_SELECT) {
                        credits += coinValue;
                        unassignedSeconds += addedSeconds;
                        drawSelectPage(); 
                    }
                    else if (currentPage == PAGE_CHARGING || currentPage == PAGE_WIFI) {
                        pendingCredits += coinValue;
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

    // --- 2. SCREEN UI & BUTTON STATE MACHINE ---
    if (currentPage == PAGE_INSERT) {
      if (millis() - lastBlink > 500) {
        lastBlink = millis();
        showText = !showText;
        tft.fillRect(20, 100, 120, 20, BG);
        if (showText) {
          tft.setCursor(25, 100);
          tft.setTextColor(HL);
          tft.print("INSERT COIN");
        }
      }
    }
    
    else if (currentPage == PAGE_SELECT) {
      if (digitalRead(BTN_1_PIN) == HIGH) {
          activateRandomPort(unassignedSeconds); 
          unassignedSeconds = 0; 
          
          currentPage = PAGE_CHARGING;
          lastSecondTick = millis(); 
          drawChargingPage();
          delay(300); 
      } 
      else if (digitalRead(BTN_2_PIN) == HIGH) {
          wifiSeconds += unassignedSeconds;
          unassignedSeconds = 0;
          
          currentPage = PAGE_WIFI;
          lastSecondTick = millis(); 
          drawWiFiPage();
          delay(300); 
      }
    }
    
    else if (currentPage == PAGE_CHARGING) {
      if (millis() - lastSecondTick >= 1000) {
          lastSecondTick = millis();
          bool anyPortActive = false;

          for(int i = 0; i < 4; i++) {
              if (portSeconds[i] > 0) {
                  portSeconds[i]--;
                  anyPortActive = true;
                  
                  if (portSeconds[i] <= 0) {
                      portActive[i] = false;
                      digitalWrite(RELAY_PINS[i], HIGH); 
                  }
              }
          }

          if (anyPortActive) {
              tft.fillRect(2, 22, 156, 100, BG); 
              int y = 30;
              for(int i = 0; i < 4; i++) {
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
              deactivateAllPorts(); 
              currentPage = PAGE_INSERT;
              drawInsertPage();
          }
      }
    }
    
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
    
    else if (currentPage == PAGE_EXTEND) {
      if (digitalRead(BTN_1_PIN) == HIGH) {
          portSeconds[lastActivatedPort] += unassignedSeconds;
          credits += pendingCredits;
          
          unassignedSeconds = 0;
          pendingCredits = 0;
          
          currentPage = PAGE_CHARGING; 
          lastSecondTick = millis(); 
          drawChargingPage();
          delay(300);
      } 
      else if (digitalRead(BTN_2_PIN) == HIGH) {
          activateRandomPort(unassignedSeconds);
          credits += pendingCredits;
          
          unassignedSeconds = 0;
          pendingCredits = 0;

          currentPage = PAGE_CHARGING;
          lastSecondTick = millis();
          drawChargingPage();
          delay(300);
      }
    }

    // --- 3. BACKGROUND TASKS ---
    update_firebase();
}