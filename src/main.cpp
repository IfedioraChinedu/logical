// src/main.cpp
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <SPI.h>
#include <MFRC522.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <ArduinoJson.h>

// ================== PIN CONFIG ==================
#define RFID_SS_PIN   5
#define RFID_RST_PIN  27
#define RFID_SCK_PIN  18
#define RFID_MOSI_PIN 23
#define RFID_MISO_PIN 19
#define BATTERY_PIN   34

// ================= OBJECTS ======================
LiquidCrystal_I2C lcd(0x27, 16, 2);
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
Preferences prefs;

// ================= BATTERY ICONS =================
byte battery0[8] = {B01110,B10001,B10001,B10001,B10001,B10001,B10001,B11111};
byte battery1[8] = {B01110,B10001,B10001,B10001,B10001,B10001,B11111,B11111};
byte battery2[8] = {B01110,B10001,B10001,B10001,B11111,B11111,B11111,B11111};
byte battery3[8] = {B01110,B10001,B10001,B11111,B11111,B11111,B11111,B11111};
byte battery4[8] = {B01110,B11111,B11111,B11111,B11111,B11111,B11111,B11111};

// ================= BACKEND CONFIG =================
const char* API_BASE = "https://djodwbvntdlamhydpuih.supabase.co/functions/v1";

const char* DEVICE_LOGIN_PATH   = "/device-login";
const char* HEARTBEAT_PATH      = "/device-heartbeat";
const char* NEW_RFID_PATH       = "/master-new-rfid";
const char* COMMANDS_PATH       = "/master-commands";
const char* COMMANDS_ACK_PATH   = "/master-commands-ack";

// ================= DEVICE IDENTITY =================
String deviceSerial = "MASTER_001";
String deviceSecret = "PMT_Nsukka_01";

String deviceId  = "";
String companyId = "";
String deviceJWT = "";
unsigned long jwtExpiry = 0;

const char* FIRMWARE_VERSION = "v1.0.0";

// ================= TIMERS =========================
unsigned long lastHeartbeatMs   = 0;
unsigned long lastCommandPollMs = 0;
unsigned long lastBatteryMs     = 0;
unsigned long lastBatteryBlinkMs = 0;
unsigned long lastRfidDebounceMs = 0;

const unsigned long HEARTBEAT_INTERVAL_MS = 60000;
const unsigned long COMMANDS_INTERVAL_MS  = 5000;
const unsigned long BATTERY_INTERVAL_MS   = 5000;
const unsigned long BATTERY_BLINK_INTERVAL_MS = 500;
const unsigned long RFID_DEBOUNCE_MS      = 600;

bool wifiConfigMode = false;

// ================= NON-BLOCKING SCROLL ==================
bool scrollActive = false;
String scrollText = "";
unsigned long scrollStartTime = 0;
unsigned long scrollLastStep = 0;
int scrollIndex = 0;

const int SCROLL_COLS = 13;  
const unsigned long SCROLL_STEP_MS = 250;
const unsigned long SCROLL_TOTAL_TIME = 3000;

// ================= UTIL FUNCTIONS =================
String urlJoin(const String &base, const String &path) {
    if (base.endsWith("/") && path.startsWith("/")) return base + path.substring(1);
    if (!base.endsWith("/") && !path.startsWith("/")) return base + "/" + path;
    return base + path;
}

void safeCenterPrintLine(int row, const String &txt) {
    int pos = (16 - txt.length()) / 2;
    if (pos < 0) pos = 0;
    lcd.setCursor(pos, row);
    lcd.print(txt);
}

void showCentered(const String &a, const String &b = "") {
    lcd.clear();
    safeCenterPrintLine(0, a);
    if (b.length()) safeCenterPrintLine(1, b);
}

// ================= BATTERY FUNCTIONS =================
float readBatteryVoltage() {
    uint16_t raw = analogRead(BATTERY_PIN);
    float v_adc = (raw * 3.3f) / 4095.0f;
    return v_adc * 2.0f;
}

int batteryPercent(float vbat) {
    const float VMIN = 3.3f;
    const float VMAX = 4.2f;
    if (vbat <= VMIN) return 0;
    if (vbat >= VMAX) return 100;
    return (int)((vbat - VMIN) * 100.0f / (VMAX - VMIN));
}

uint8_t batteryCharIndexFromPct(int pct) {
    if (pct <= 5) return 0;
    if (pct <= 25) return 1;
    if (pct <= 50) return 2;
    if (pct <= 75) return 3;
    return 4;
}

bool batteryBlinkState = false;

void drawBatteryIcon(int pct) {
    uint8_t idx = batteryCharIndexFromPct(pct);

    unsigned long now = millis();
    if (pct < 15) {
        if (now - lastBatteryBlinkMs >= BATTERY_BLINK_INTERVAL_MS) {
            lastBatteryBlinkMs = now;
            batteryBlinkState = !batteryBlinkState;
        }
    } else {
        batteryBlinkState = true;
    }

    lcd.setCursor(15, 0);
    if (batteryBlinkState) lcd.write(idx);
    else lcd.print(" ");
}

// ================= STORAGE =================
void loadAuth() {
    deviceId  = prefs.getString("deviceId", "");
    companyId = prefs.getString("companyId", "");
    deviceJWT = prefs.getString("deviceJWT", "");
    jwtExpiry = prefs.getULong("jwtExp", 0);
}

void saveAuth() {
    prefs.putString("deviceId", deviceId);
    prefs.putString("companyId", companyId);
    prefs.putString("deviceJWT", deviceJWT);
    prefs.putULong("jwtExp", jwtExpiry);
}

// ================= HTTP HELPERS =================
bool httpPostJson(const String &fullUrl, const String &body, String &response, bool auth = true) {
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    http.begin(fullUrl);
    http.addHeader("Content-Type", "application/json");
    if (auth && deviceJWT.length() > 0)
        http.addHeader("Authorization", "Bearer " + deviceJWT);

    int code = http.POST(body);
    if (code <= 0) { http.end(); return false; }

    response = http.getString();
    http.end();
    return (code >= 200 && code < 300);
}

bool httpGet(const String &fullUrl, String &response, bool auth = true) {
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    http.begin(fullUrl);
    if (auth && deviceJWT.length() > 0)
        http.addHeader("Authorization", "Bearer " + deviceJWT);

    int code = http.GET();
    if (code <= 0) { http.end(); return false; }

    response = http.getString();
    http.end();
    return (code >= 200 && code < 300);
}

// ================= LOGIN =================
bool deviceLogin() {
    String url = urlJoin(API_BASE, DEVICE_LOGIN_PATH);

    StaticJsonDocument<256> doc;
    doc["device_serial"] = deviceSerial;
    doc["device_type"] = "master";
    doc["device_secret"] = deviceSecret;
    String body; serializeJson(doc, body);

    String res;
    if (!httpPostJson(url, body, res, false)) return false;

    StaticJsonDocument<512> r;
    if (deserializeJson(r, res)) return false;

    if (r.containsKey("error")) return false;

    deviceId  = r["device_id"].as<String>();
    companyId = r["company_id"].as<String>();
    deviceJWT = r["jwt"].as<String>();
    jwtExpiry = r["jwt_exp"] | 0;

    saveAuth();
    return true;
}

bool shouldRefreshJWT() {
    if (jwtExpiry == 0) return true;
    unsigned long nowSec = millis() / 1000UL;
    return (nowSec + 3600UL >= jwtExpiry);
}

// ================= HEARTBEAT =================
void sendHeartbeat() {
    if (deviceId.length() == 0) return;

    String url = urlJoin(API_BASE, HEARTBEAT_PATH);

    StaticJsonDocument<128> d;
    d["device_id"] = deviceId;
    d["device_type"] = "master";
    d["firmware_version"] = FIRMWARE_VERSION;
    String body; serializeJson(d, body);

    String res;
    if (!httpPostJson(url, body, res, true)) {
        if (shouldRefreshJWT()) deviceLogin();
    }
}

// ================= COMMANDS =================
void ackCommand(const String &cmdId) {
    String url = urlJoin(API_BASE, COMMANDS_ACK_PATH);

    StaticJsonDocument<128> d;
    d["command_id"] = cmdId;
    String body; serializeJson(d, body);

    String res;
    httpPostJson(url, body, res, true);
}

void handleCommandStartWiFiManager(const String &cmdId) {
    ackCommand(cmdId);
    showCentered("WiFi Reset", "Config Portal");
    delay(800);

    WiFiManager wm;
    String apName = deviceSerial + "_setup";
    wifiConfigMode = true;

    wm.startConfigPortal(apName.c_str());  

    ESP.restart();
}

void pollCommands() {
    if (deviceId.length() == 0) return;

    String url = urlJoin(API_BASE, COMMANDS_PATH);
    url += "?device_id=" + deviceId;

    String res;
    if (!httpGet(url, res, true)) return;

    StaticJsonDocument<1024> doc;
    if (deserializeJson(doc, res)) return;

    if (!doc.containsKey("commands")) return;
    JsonArray arr = doc["commands"];

    for (JsonObject cmd : arr) {
        String id = cmd["id"].as<String>();
        String action = cmd["action"].as<String>();

        if (action == "start_wifi_manager") handleCommandStartWiFiManager(id);
        else ackCommand(id);
    }
}

// ================= RFID =================
String uidHexFromMFRC() {
    String uid = "";
    for (byte i=0; i<rfid.uid.size; i++) {
        if (rfid.uid.uidByte[i] < 0x10) uid += "0";
        uid += String(rfid.uid.uidByte[i], HEX);
    }
    uid.toUpperCase();
    return uid;
}

// =========== NON-BLOCKING CUSTOMER SCROLL ===========
void startCustomerDisplay(const String &name, const String &wallet) {
    lcd.clear();

    lcd.setCursor(0,1);
    lcd.print("Bal: # ");
    lcd.print(wallet);

    float v = readBatteryVoltage();
    drawBatteryIcon(batteryPercent(v));

    scrollActive = (name.length() > SCROLL_COLS);

    if (!scrollActive) {
        lcd.setCursor(0,0);
        lcd.print(name);
        lcd.setCursor(13,0);
        lcd.print(" ");
        return;
    }

    scrollText = name + "   ";
    scrollIndex = 0;
    scrollStartTime = millis();
    scrollLastStep = millis();

    lcd.setCursor(0,0);
    lcd.print(name.substring(0, SCROLL_COLS));
    lcd.print(" ");
}

void updateScroll() {
    if (!scrollActive) return;

    unsigned long now = millis();
    if (now - scrollStartTime > SCROLL_TOTAL_TIME) {
        scrollActive = false;
        return;
    }

    if (now - scrollLastStep > SCROLL_STEP_MS) {
        scrollLastStep = now;

        String frame = "";
        for (int i = 0; i < SCROLL_COLS; i++) {
            frame += scrollText[(scrollIndex + i) % scrollText.length()];
        }
        scrollIndex = (scrollIndex + 1) % scrollText.length();

        lcd.setCursor(0,0);
        lcd.print(frame);
        lcd.print(" ");

        float v = readBatteryVoltage();
        drawBatteryIcon(batteryPercent(v));
    }
}

// ================= PROCESS RFID =================
void processScannedUID(const String &uid) {
    unsigned long now = millis();
    if (now - lastRfidDebounceMs < RFID_DEBOUNCE_MS) return;
    lastRfidDebounceMs = now;

    showCentered("Card UID:", uid);
    delay(300);

    StaticJsonDocument<256> d;
    d["rfid_uid"] = uid;
    d["master_device_id"] = deviceId;
    String body; serializeJson(d, body);

    String url = urlJoin(API_BASE, NEW_RFID_PATH);
    String res;
    bool ok = httpPostJson(url, body, res, true);

    if (!ok) {
        if (res.indexOf("Invalid") >= 0) deviceLogin();
        return;
    }

    StaticJsonDocument<1024> r;
    if (deserializeJson(r, res)) {
        showCentered("Card Sent", uid);
        return;
    }

    if (r.containsKey("customer")) {
        JsonObject cust = r["customer"].as<JsonObject>();
        String name = cust["name"] | "Unknown";

        long wallet_balance = 0;
        if (cust.containsKey("wallet")) wallet_balance = cust["wallet"].as<long>();
        if (cust.containsKey("wallet_balance")) wallet_balance = cust["wallet_balance"].as<long>();

        char buf[32];
        sprintf(buf, "%.2f", (float)wallet_balance); 
        String walletFormatted = String(buf);

        startCustomerDisplay(name, walletFormatted);
        return;
    }

    if (r.containsKey("success") && r["success"].as<bool>() == true) {
        lcd.clear();
        safeCenterPrintLine(0, "NEW RFID TAG");
        safeCenterPrintLine(1, uid.substring(0,16));
        float v = readBatteryVoltage();
        drawBatteryIcon(batteryPercent(v));
        delay(800);
        return;
    }

    showCentered("Card Sent", uid);
}

// ================= WIFI PORTAL =================
void startWiFiPortal() {
    wifiConfigMode = true;
    WiFiManager wm;

    String apName = deviceSerial + "_setup";
    bool res = wm.startConfigPortal(apName.c_str());  

    wifiConfigMode = false;
}

// ================= INTERNET CHECK =================
void checkInternet() {
    if (WiFi.status() != WL_CONNECTED) return;

    HTTPClient http;
    http.begin("http://clients3.google.com/generate_204");
    http.setTimeout(3000);
    int code = http.GET();
    http.end();

    if (code != 204) {
        lcd.clear();
        safeCenterPrintLine(0, "Poor Internet");
        float v = readBatteryVoltage();
        drawBatteryIcon(batteryPercent(v));
        delay(300);
    }
}

// ================= SETUP =================
void setup() {
    Serial.begin(115200);
    prefs.begin("master", false);

    lcd.init();
    lcd.backlight();
    lcd.clear();

    lcd.createChar(0, battery0);
    lcd.createChar(1, battery1);
    lcd.createChar(2, battery2);
    lcd.createChar(3, battery3);
    lcd.createChar(4, battery4);

    showCentered("Booting...");

    analogReadResolution(12);
    analogSetPinAttenuation(BATTERY_PIN, ADC_11db);

    SPI.begin(RFID_SCK_PIN, RFID_MISO_PIN, RFID_MOSI_PIN, RFID_SS_PIN);
    rfid.PCD_Init();

    loadAuth();

    WiFiManager wm;
    String apName = deviceSerial + "_setup";

    bool connected = wm.autoConnect(apName.c_str(), "");
    if (!connected) {
        showCentered("WiFi Failed", "Restarting");
        delay(1000);
        ESP.restart();
    }

    showCentered("WiFi OK", WiFi.localIP().toString());
    delay(400);

    int attempts = 0;
    while (!deviceLogin() && attempts < 5) {
        attempts++;
        delay(1500);
    }

    if (attempts >= 5) {
        showCentered("Login Failed");
        while (true) delay(1000);
    }

    showCentered("Master Ready", deviceSerial);
    delay(300);
}

// ================= LOOP =================
void loop() {
    unsigned long now = millis();

    updateScroll();

    if (WiFi.status() != WL_CONNECTED && !wifiConfigMode) {
        showCentered("WiFi Lost", "Setup Portal");
        startWiFiPortal();
    }

    if (now - lastBatteryMs > BATTERY_INTERVAL_MS) {
        lastBatteryMs = now;
        float v = readBatteryVoltage();
        drawBatteryIcon(batteryPercent(v));
    }

    if (now - lastHeartbeatMs > HEARTBEAT_INTERVAL_MS) {
        lastHeartbeatMs = now;
        sendHeartbeat();
    }

    if (now - lastCommandPollMs > COMMANDS_INTERVAL_MS) {
        lastCommandPollMs = now;
        pollCommands();
    }

    checkInternet();

    if (wifiConfigMode) {
        delay(50);
        return;
    }

    if (!rfid.PICC_IsNewCardPresent()) return;
    if (!rfid.PICC_ReadCardSerial()) return;

    String uid = uidHexFromMFRC();
    processScannedUID(uid);

    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
}
