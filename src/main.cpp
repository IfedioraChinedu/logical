#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>

// ================== RFID PINS ==================
#define RFID_SS_PIN   5    // SDA / SS (corrected)
#define RFID_RST_PIN  27
#define RFID_SCK_PIN  18
#define RFID_MOSI_PIN 23
#define RFID_MISO_PIN 19

// ================== BATTERY PIN ================
#define BATTERY_PIN   34   // ADC from 2x 2.2k divider

// ================== LCD ========================
LiquidCrystal_I2C lcd(0x27, 16, 2);

// =============== BATTERY ICONS ================
byte battery0[8] = {B01110,B10001,B10001,B10001,B10001,B10001,B10001,B11111};
byte battery1[8] = {B01110,B10001,B10001,B10001,B10001,B10001,B11111,B11111};
byte battery2[8] = {B01110,B10001,B10001,B10001,B10001,B11111,B11111,B11111};
byte battery3[8] = {B01110,B10001,B10001,B10001,B11111,B11111,B11111,B11111};
byte battery4[8] = {B01110,B10001,B10001,B11111,B11111,B11111,B11111,B11111};

// =============== BACKEND CONFIG ===============
const char* API_BASE = "https://your-backend-url.com";   // TODO: CHANGE THIS

const char* DEVICE_LOGIN_PATH   = "/api/device/login";
const char* HEARTBEAT_PATH      = "/api/device/heartbeat";
const char* NEW_RFID_PATH       = "/api/master/new-rfid";
const char* COMMANDS_PATH       = "/api/master/commands";
const char* COMMANDS_ACK_PATH   = "/api/master/commands/ack";

// =============== DEVICE IDENTITY ===============
Preferences prefs;

String deviceSerial = "MASTER_001";
String deviceType   = "master";

String deviceId  = "";
String companyId = "";
String deviceJWT = "";

// Stored WiFi credentials
String wifiSSID = "";
String wifiPass = "";

// ================= RFID ========================
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
String lastSentUID = "";

// ================== TIMERS =====================
unsigned long lastHeartbeatMs   = 0;
unsigned long lastBatteryMs     = 0;
unsigned long lastCommandPollMs = 0;

const unsigned long HEARTBEAT_INTERVAL_MS = 60000;
const unsigned long BATTERY_INTERVAL_MS   = 5000;
const unsigned long COMMANDS_INTERVAL_MS  = 5000;

// =============== WIFI MANAGER ================
WebServer server(80);
bool wifiConfigMode = false;

// =============== UTILS =======================

String getJsonString(const String& json, const String& key) {
  String pattern = "\"" + key + "\":";
  int idx = json.indexOf(pattern);
  if (idx < 0) return "";

  idx += pattern.length();
  while (json[idx] == ' ') idx++;

  if (json[idx] == '\"') {
    int start = idx + 1;
    int end = json.indexOf('\"', start);
    return json.substring(start, end);
  }

  int start = idx;
  int end = json.indexOf(',', start);
  if (end < 0) end = json.indexOf('}', start);
  return json.substring(start, end);
}

void showCentered(String a, String b = "") {
  lcd.clear();
  int s = (16 - a.length()) / 2;
  lcd.setCursor(max(0, s), 0);
  lcd.print(a);

  if (b.length()) {
    int s2 = (16 - b.length()) / 2;
    lcd.setCursor(max(0, s2), 1);
    lcd.print(b);
  }
}

// =============== BATTERY SYSTEM ===============

bool blinkState = false;

float readBatteryVoltage() {
  int raw = analogRead(BATTERY_PIN);
  float v_adc = raw * 3.3 / 4095.0;
  return v_adc * 2.0;
}

int batteryPercent(float vbat) {
  const float VMIN = 3.3;
  const float VMAX = 4.2;
  if (vbat <= VMIN) return 0;
  if (vbat >= VMAX) return 100;
  return (vbat - VMIN) * 100 / (VMAX - VMIN);
}

void updateBatteryIcon() {
  float vbat = readBatteryVoltage();
  int pct = batteryPercent(vbat);
  byte icon;

  if (pct <= 5)       icon = 0;
  else if (pct <= 25) icon = 1;
  else if (pct <= 50) icon = 2;
  else if (pct <= 75) icon = 3;
  else                icon = 4;

  lcd.setCursor(15, 0);

  if (pct < 20) {
    blinkState = !blinkState;
    if (blinkState)
      lcd.write(icon);
    else
      lcd.print(" ");
  } else {
    lcd.write(icon);
  }
}

// =============== NVS LOAD =====================

void loadWifi() {
  wifiSSID = prefs.getString("wifi_ssid", "");
  wifiPass = prefs.getString("wifi_pass", "");
}

void saveWifi(String ssid, String pass) {
  prefs.putString("wifi_ssid", ssid);
  prefs.putString("wifi_pass", pass);
  wifiSSID = ssid;
  wifiPass = pass;
}

void loadAuth() {
  deviceId  = prefs.getString("deviceId", "");
  companyId = prefs.getString("companyId", "");
  deviceJWT = prefs.getString("deviceJWT", "");
}

// =============== HTTP HELPERS =================

bool httpPostJson(const char* path, String body, String &response, bool auth = true) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.begin(String(API_BASE) + path);
  http.addHeader("Content-Type", "application/json");

  if (auth && deviceJWT.length())
    http.addHeader("Authorization", "Bearer " + deviceJWT);

  int code = http.POST(body);
  if (code <= 0) {
    http.end();
    return false;
  }

  response = http.getString();
  http.end();
  return (code >= 200 && code < 300);
}

bool httpGet(String path, String &response) {
  if (WiFi.status() != WL_CONNECTED) return false;

  HTTPClient http;
  http.begin(path);

  if (deviceJWT.length())
    http.addHeader("Authorization", "Bearer " + deviceJWT);

  int code = http.GET();
  if (code <= 0) {
    http.end();
    return false;
  }

  response = http.getString();
  http.end();
  return (code >= 200 && code < 300);
}

// =============== WIFI CONFIG PORTAL ==========

void handleRoot() {
  String html = R"(
  <html><body>
  <h2>WiFi Setup</h2>
  <form action="/savewifi" method="POST">
  SSID:<br><input name="ssid"><br><br>
  Password:<br><input name="pass" type="password"><br><br>
  <button>Save & Reboot</button>
  </form>
  </body></html>
  )";
  server.send(200, "text/html", html);
}

void handleSaveWifi() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");

  saveWifi(ssid, pass);

  server.send(200, "text/plain", "Saved! Rebooting...");
  delay(800);
  ESP.restart();
}

void startWiFiAP(String reason) {
  wifiConfigMode = true;

  WiFi.mode(WIFI_AP);
  String apName = deviceSerial;
  WiFi.softAP(apName.c_str(), NULL);

  showCentered("WiFi Setup", apName);

  server.on("/", handleRoot);
  server.on("/savewifi", HTTP_POST, handleSaveWifi);
  server.begin();

  Serial.println("AP Mode ON: " + apName + " Reason: " + reason);
}

bool connectWiFiStored() {
  if (wifiSSID.length() == 0) return false;

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());

  showCentered("Connecting...", wifiSSID);

  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000)
    delay(300);

  if (WiFi.status() == WL_CONNECTED) {
    showCentered("WiFi OK");
    delay(800);
    return true;
  }

  return false;
}

// =============== LOGIN & HEARTBEAT ==========

bool deviceLogin() {
  String body = "{\"device_serial\":\"" + deviceSerial +
                "\",\"device_type\":\"master\"}";

  String res;
  if (!httpPostJson(DEVICE_LOGIN_PATH, body, res, false)) return false;

  deviceId  = getJsonString(res, "device_id");
  companyId = getJsonString(res, "company_id");
  deviceJWT = getJsonString(res, "jwt");

  prefs.putString("deviceId", deviceId);
  prefs.putString("companyId", companyId);
  prefs.putString("deviceJWT", deviceJWT);

  showCentered("Login OK");
  delay(600);
  return true;
}

void sendHeartbeat() {
  if (deviceId == "" || deviceJWT == "") return;

  String body = "{\"device_id\":\"" + deviceId +
                "\",\"device_type\":\"master\",\"firmware_version\":\"1.0.0\"}";

  String res;
  httpPostJson(HEARTBEAT_PATH, body, res, true);
}

// =============== COMMANDS ====================

void ackCommand(String id) {
  String body = "{\"command_id\":\"" + id + "\"}";
  String res;
  httpPostJson(COMMANDS_ACK_PATH, body, res, true);
}

void pollCommands() {
  String url = String(API_BASE) + COMMANDS_PATH + "?device_id=" + deviceId;

  String res;
  if (!httpGet(url, res)) return;

  int idx = res.indexOf("start_wifi_manager");
  if (idx >= 0) {
    int idStart = res.indexOf("\"id\":\"");
    int idEnd = res.indexOf("\"", idStart + 6);
    String cmdId = res.substring(idStart + 6, idEnd);

    ackCommand(cmdId);
    startWiFiAP("command");
  }
}

// =============== RFID ========================

String uidHex() {
  String uid = "";
  for (byte i=0; i<rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

void sendRFID(String uid) {
  String body = "{\"rfid_uid\":\"" + uid +
                "\",\"master_device_id\":\"" + deviceId + "\"}";

  String res;
  httpPostJson(NEW_RFID_PATH, body, res, true);
}

// =============== SETUP =======================

void setup() {
  Serial.begin(115200);
  prefs.begin("master", false);

  lcd.init();
  lcd.backlight();
  lcd.print("Booting...");

  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_PIN, ADC_11db);

  lcd.createChar(0, battery0);
  lcd.createChar(1, battery1);
  lcd.createChar(2, battery2);
  lcd.createChar(3, battery3);
  lcd.createChar(4, battery4);

  SPI.begin(RFID_SCK_PIN, RFID_MISO_PIN, RFID_MOSI_PIN, RFID_SS_PIN);
  rfid.PCD_Init();

  loadWifi();
  loadAuth();

  if (!connectWiFiStored())
    startWiFiAP("no saved WiFi");
  else {
    if (deviceJWT == "" || deviceId == "")
      deviceLogin();

    showCentered("Master Ready");
  }
}

// =============== LOOP ========================

void loop() {
  unsigned long now = millis();

  if (wifiConfigMode) {
    server.handleClient();
    if (now - lastBatteryMs > BATTERY_INTERVAL_MS) {
      lastBatteryMs = now;
      updateBatteryIcon();
    }
    return;
  }

  // Battery
  if (now - lastBatteryMs > BATTERY_INTERVAL_MS) {
    lastBatteryMs = now;
    updateBatteryIcon();
  }

  // Heartbeat
  if (now - lastHeartbeatMs > HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMs = now;
    sendHeartbeat();
  }

  // Command polling
  if (now - lastCommandPollMs > COMMANDS_INTERVAL_MS) {
    lastCommandPollMs = now;
    pollCommands();
  }

  // RFID
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial())   return;

  String uid = uidHex();

  if (uid != lastSentUID) {
    lastSentUID = uid;
    showCentered("Card UID:", uid);
    delay(400);
    sendRFID(uid);
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}