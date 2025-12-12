// src/main.cpp
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiManager.h>           // tzapu WiFiManager
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
#define BATTERY_PIN   34   // ADC pin (use your divider middle point)

// ================= OBJECTS ======================
LiquidCrystal_I2C lcd(0x27, 16, 2);
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
Preferences prefs;

// =============== BATTERY ICONS (user provided) ===============
byte battery0[8] = {B01110,B10001,B10001,B10001,B10001,B10001,B10001,B11111};
byte battery1[8] = {B01110,B10001,B10001,B10001,B10001,B10001,B11111,B11111};
byte battery2[8] = {B01110,B10001,B10001,B10001,B11111,B11111,B11111,B11111};
byte battery3[8] = {B01110,B10001,B10001,B11111,B11111,B11111,B11111,B11111};
byte battery4[8] = {B01110,B11111,B11111,B11111,B11111,B11111,B11111,B11111};

// ================= BACKEND CONFIG / TODO =================
// TODO: set this to your Supabase Edge Functions base URL (no trailing slash)
const char* API_BASE = "https://djodwbvntdlamhydpuih.supabase.co/functions/v1"; // <--- CHANGE THIS

// Edge function paths (appended to API_BASE)
const char* DEVICE_LOGIN_PATH   = "/device-login";
const char* HEARTBEAT_PATH      = "/device-heartbeat";
const char* NEW_RFID_PATH       = "/master-new-rfid";
const char* COMMANDS_PATH       = "/master-commands";
const char* COMMANDS_ACK_PATH   = "/master-commands-ack";

// ================= DEVICE IDENTITY (TODO) =================
// TODO: replace with your device serial exactly as registered in DB
String deviceSerial = "MASTER_001";          // <--- CHANGE THIS
// TODO: replace with actual device secret you registered in master_devices.device_secret
String deviceSecret = "PMT_Nsukka_01";      // <--- CHANGE THIS

String deviceId  = "";
String companyId = "";
String deviceJWT = "";
unsigned long jwtExpiry = 0; // epoch seconds

const char* FIRMWARE_VERSION = "v1.0.0";

// ================== TIMERS =======================
unsigned long lastHeartbeatMs   = 0;
unsigned long lastCommandPollMs = 0;
unsigned long lastBatteryMs     = 0;
unsigned long lastRfidDebounceMs = 0;

const unsigned long HEARTBEAT_INTERVAL_MS = 60000;  // 60s
const unsigned long COMMANDS_INTERVAL_MS  = 5000;   // 5s
const unsigned long BATTERY_INTERVAL_MS   = 5000;   // 5s
const unsigned long RFID_DEBOUNCE_MS      = 600;    // 600ms

bool wifiConfigMode = false;
String lastSentUID = "";

// ================== UTIL FUNCTIONS ====================

String urlJoin(const String &base, const String &path) {
  if (base.endsWith("/") && path.startsWith("/")) return base + path.substring(1);
  if (!base.endsWith("/") && !path.startsWith("/")) return base + "/" + path;
  return base + path;
}

void safeCenterPrintLine(int row, const String &txt) {
  int len = txt.length();
  int pos = (16 - len) / 2;
  if (pos < 0) pos = 0;
  lcd.setCursor(pos, row);
  lcd.print(txt);
}

void showCentered(const String &a, const String &b = "") {
  lcd.clear();
  safeCenterPrintLine(0, a);
  if (b.length()) safeCenterPrintLine(1, b);
}

// ================== BATTERY =====================

float readBatteryVoltage() {
  // ADC read: 0..4095, Vref=3.3 by default (may vary per board)
  uint16_t raw = analogRead(BATTERY_PIN);
  float v_adc = (raw * 3.3f) / 4095.0f;
  // Divider 2.2k/2.2k => multiply by 2
  return v_adc * 2.0f;
}

int batteryPercent(float vbat) {
  const float VMIN = 3.3f; // TODO: adjust if your cutoff differs
  const float VMAX = 4.2f;
  if (vbat <= VMIN) return 0;
  if (vbat >= VMAX) return 100;
  return (int)((vbat - VMIN) * 100.0f / (VMAX - VMIN));
}

void drawBatteryIcon(int pct) {
  // map pct to 0..4
  int idx = 0;
  if (pct <= 5) idx = 0;
  else if (pct <= 25) idx = 1;
  else if (pct <= 50) idx = 2;
  else if (pct <= 75) idx = 3;
  else idx = 4;

  // print at top-right position
  lcd.setCursor(15, 0); // rightmost char
  lcd.write((uint8_t)idx);
}

// ================== NVS =====================

void loadAuth() {
  deviceId  = prefs.getString("deviceId", "");
  companyId = prefs.getString("companyId", "");
  deviceJWT = prefs.getString("deviceJWT", "");
  jwtExpiry = prefs.getULong("jwtExp", 0);
  Serial.println("NVS: deviceId len=" + String(deviceId.length()) + " jwt len=" + String(deviceJWT.length()));
}

void saveAuth() {
  prefs.putString("deviceId", deviceId);
  prefs.putString("companyId", companyId);
  prefs.putString("deviceJWT", deviceJWT);
  prefs.putULong("jwtExp", jwtExpiry);
}

// ================== HTTP HELPERS =================

bool httpPostJson(const String &fullUrl, const String &body, String &response, bool auth = true) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("HTTP: WiFi not connected");
    return false;
  }

  HTTPClient http;
  http.begin(fullUrl);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(15000);
  if (auth && deviceJWT.length() > 0) {
    http.addHeader("Authorization", "Bearer " + deviceJWT);
  }

  int code = http.POST(body);
  if (code <= 0) {
    Serial.println("HTTP POST failed: " + http.errorToString(code));
    http.end();
    return false;
  }

  response = http.getString();
  Serial.println("HTTP POST " + String(code) + " -> " + response);
  http.end();
  return (code >= 200 && code < 300);
}

bool httpGet(const String &fullUrl, String &response, bool auth = true) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("HTTP: WiFi not connected");
    return false;
  }
  HTTPClient http;
  http.begin(fullUrl);
  http.setTimeout(10000);
  if (auth && deviceJWT.length() > 0) {
    http.addHeader("Authorization", "Bearer " + deviceJWT);
  }
  int code = http.GET();
  if (code <= 0) {
    Serial.println("HTTP GET failed: " + http.errorToString(code));
    http.end();
    return false;
  }
  response = http.getString();
  Serial.println("HTTP GET " + String(code) + " -> " + response);
  http.end();
  return (code >= 200 && code < 300);
}

// ================== DEVICE LOGIN =================

bool deviceLogin() {
  String url = urlJoin(API_BASE, DEVICE_LOGIN_PATH);
  StaticJsonDocument<256> doc;
  doc["device_serial"] = deviceSerial;
  doc["device_type"] = "master";
  doc["device_secret"] = deviceSecret;
  String body;
  serializeJson(doc, body);

  Serial.println("=== DEVICE LOGIN ===");
  Serial.println("POST " + url);
  Serial.println("Body: " + body);

  String res;
  if (!httpPostJson(url, body, res, false)) {
    Serial.println("❌ HTTP POST failed");
    return false;
  }

  // parse response
  StaticJsonDocument<512> r;
  DeserializationError err = deserializeJson(r, res);
  if (err) {
    Serial.println("❌ JSON parse error: " + String(err.c_str()));
    return false;
  }

  if (r.containsKey("error")) {
    String errMsg = r["error"].as<String>();
    Serial.println("❌ Backend error: " + errMsg);
    if (r.containsKey("locked_until")) {
      String lu = r["locked_until"].as<String>();
      showCentered("LOCKED", lu.substring(11, 16));
    } else if (errMsg == "Invalid credentials") {
      int remaining = r["attempts_remaining"] | 0;
      showCentered("Bad Secret", String(remaining) + " tries left");
    } else if (errMsg == "Device not found") {
      showCentered("Not Found", "Check Serial");
    } else if (errMsg == "Device not registered") {
      showCentered("Not Registered", "Admin needed");
    }
    return false;
  }

  deviceId  = r["device_id"].as<String>();
  companyId = r["company_id"].as<String>();
  deviceJWT = r["jwt"].as<String>();
  jwtExpiry = r["jwt_exp"] | 0;

  if (deviceId.length() == 0 || deviceJWT.length() == 0) {
    Serial.println("❌ Missing fields in login response");
    return false;
  }

  saveAuth();
  Serial.println("✅ Login SUCCESS");
  Serial.println("Device ID: " + deviceId);
  Serial.println("Company ID: " + companyId);
  Serial.println("JWT exp (raw): " + String(jwtExpiry));
  showCentered("Login OK", deviceSerial);
  delay(600);
  return true;
}

bool shouldRefreshJWT() {
  if (jwtExpiry == 0) return true;
  unsigned long nowSec = millis() / 1000UL;
  // Refresh if within 1 hour of expiry
  if (nowSec + 3600UL >= jwtExpiry) return true;
  return false;
}

// ================== HEARTBEAT ====================

void sendHeartbeat() {
  if (deviceId.length() == 0 || deviceJWT.length() == 0) return;
  String url = urlJoin(API_BASE, HEARTBEAT_PATH);
  StaticJsonDocument<128> d;
  d["device_id"] = deviceId;
  d["device_type"] = "master";
  d["firmware_version"] = FIRMWARE_VERSION;
  String body; serializeJson(d, body);

  String res;
  if (!httpPostJson(url, body, res, true)) {
    Serial.println("Heartbeat failed; maybe JWT expired. Attempting relogin.");
    if (shouldRefreshJWT()) {
      if (deviceLogin()) {
        // login ok, continue
      }
    }
    return;
  }

  // We could parse server response for pending commands count if needed
}

// ================== COMMANDS ====================

void ackCommand(const String &cmdId) {
  String url = urlJoin(API_BASE, COMMANDS_ACK_PATH);
  StaticJsonDocument<128> d;
  d["command_id"] = cmdId;
  String body; serializeJson(d, body);
  String res;
  httpPostJson(url, body, res, true);
}

void handleCommandStartWiFiManager(const String &cmdId) {
  // ack then start AP config
  ackCommand(cmdId);
  showCentered("WiFi Reset", "Config Portal");
  delay(800);
  // Reset saved WiFi & enter config portal
  WiFiManager wm;
  wm.resetSettings();
  delay(500);
  // Start portal (blocks) - will open captive portal
  String apName = deviceSerial + String("_setup");
  wifiConfigMode = true;
  wm.startConfigPortal(apName.c_str()); // this opens AP and captive portal
  // After portal exits, reboot
  ESP.restart();
}

void handleCommandStartRfidScan(const String &cmdId) {
  ackCommand(cmdId);
  showCentered("Scan RFID", "Present Card");
  // simply show the prompt - RFID loop will pick up
}

void pollCommands() {
  if (deviceId.length() == 0 || deviceJWT.length() == 0) return;
  String url = urlJoin(API_BASE, COMMANDS_PATH);
  // add device_id param to be strict
  url += "?device_id=" + deviceId;

  String res;
  if (!httpGet(url, res, true)) return;

  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, res);
  if (err) {
    // not JSON
    return;
  }

  if (!doc.containsKey("commands")) return;
  JsonArray arr = doc["commands"].as<JsonArray>();
  for (JsonObject cmd : arr) {
    String cmdId = cmd["id"].as<String>();
    String action = cmd["action"].as<String>();
    Serial.println("CMD: " + action + " id=" + cmdId);
    if (action == "start_wifi_manager") {
      handleCommandStartWiFiManager(cmdId);
    } else if (action == "start_rfid_scan") {
      handleCommandStartRfidScan(cmdId);
    } else {
      // acknowledge unknown commands to avoid retry loops
      ackCommand(cmdId);
    }
  }
}

// ================== RFID ========================

String uidHexFromMFRC() {
  String uid = "";
  for (byte i=0; i<rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

void processScannedUID(const String &uid) {
  // Avoid sending the same UID repeatedly if tag stays on reader
  unsigned long now = millis();
  if (now - lastRfidDebounceMs < RFID_DEBOUNCE_MS) return;
  lastRfidDebounceMs = now;

  showCentered("Card UID:", uid);
  delay(300);

  // Build body
  StaticJsonDocument<256> d;
  d["rfid_uid"] = uid;
  d["master_device_id"] = deviceId;
  String body; serializeJson(d, body);

  String url = urlJoin(API_BASE, NEW_RFID_PATH);
  String res;
  bool ok = httpPostJson(url, body, res, true);

  if (!ok) {
    // try to detect 401 and re-login
    Serial.println("❌ Failed to send RFID. Response: " + res);
    // if invalid token, try re-login
    if (res.indexOf("Invalid or expired token") >= 0 || res.indexOf("Invalid signature") >= 0) {
      Serial.println("Token invalid; attempting relogin...");
      if (deviceLogin()) {
        // retry once
        ok = httpPostJson(url, body, res, true);
      }
    }
  }

  if (!ok) {
    Serial.println("❌ Could not send RFID after retry");
    showCentered("Send Failed", uid);
    return;
  }

  // Parse backend response and show user-friendly output
  StaticJsonDocument<512> r;
  DeserializationError err = deserializeJson(r, res);
  if (err) {
    Serial.println("❌ JSON parse error on response: " + String(err.c_str()));
    showCentered("Card Sent", uid);
    return;
  }

  // Case: already registered -> backend returns success=false and "customer" object
  if (r.containsKey("customer")) {
    JsonObject cust = r["customer"].as<JsonObject>();
    String name = cust["name"] | "Unknown";
    // wallet might be in cents — your backend decides. Example field wallet_balance
    long wallet_balance = 0;
    if (cust.containsKey("wallet")) wallet_balance = cust["wallet"].as<long>();
    if (cust.containsKey("wallet_balance")) wallet_balance = cust["wallet_balance"].as<long>();
    // Format wallet (assume in kobo maybe — your integration)
    String wb = String(wallet_balance);
    // Display name and wallet (top-right battery will remain)
    lcd.clear();
    // line 0: name (truncate/pad)
    String nameLine = name;
    if (nameLine.length() > 16) nameLine = nameLine.substring(0, 16);
    lcd.setCursor(0,0);
    lcd.print(nameLine);
    // line1: wallet
    String balLine = "Bal: " + wb;
    if (balLine.length() > 16) balLine = balLine.substring(0,16);
    lcd.setCursor(0,1);
    lcd.print(balLine);
    // draw battery icon
    float v = readBatteryVoltage();
    drawBatteryIcon(batteryPercent(v));
    delay(1500);
    return;
  }

  // Case: backend accepted and recorded scan (new RFID)
  if (r.containsKey("success") && r["success"].as<bool>() == true) {
    String msg = r["message"] | "Recorded";
    lcd.clear();
    safeCenterPrintLine(0, "NEW RFID TAG");
    // show UID truncated if necessary
    String shortUid = uid;
    if (shortUid.length() > 16) shortUid = shortUid.substring(0,16);
    safeCenterPrintLine(1, shortUid);
    // battery icon
    float v = readBatteryVoltage();
    drawBatteryIcon(batteryPercent(v));
    delay(1000);
    return;
  }

  // Otherwise fallback
  if (r.containsKey("message")) {
    String msg = r["message"].as<String>();
    showCentered("Info", String(msg));
  } else {
    showCentered("Card Sent", uid);
  }
}

// ================== WIFI SETUP =================

void startWiFiPortal() {
  wifiConfigMode = true;
  WiFiManager wm;
  // Make AP name: deviceSerial_setup
  String apName = deviceSerial + String("_setup");
  // Open AP (no password) - autoConnect with empty password opens open AP
  bool res = wm.autoConnect(apName.c_str(), "");
  if (!res) {
    showCentered("WiFi Failed", "Restarting");
    delay(2000);
    ESP.restart();
  }
  // on success, we have WiFi connected (wm.autoConnect will block until credentials provided)
  wifiConfigMode = false;
}

// ================== SETUP =======================

void setup() {
  Serial.begin(115200);
  delay(200);
  prefs.begin("master", false);

  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.createChar(0, battery0);
  lcd.createChar(1, battery1);
  lcd.createChar(2, battery2);
  lcd.createChar(3, battery3);
  lcd.createChar(4, battery4);

  showCentered("Booting...", "");

  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_PIN, ADC_11db);

  SPI.begin(RFID_SCK_PIN, RFID_MISO_PIN, RFID_MOSI_PIN, RFID_SS_PIN);
  rfid.PCD_Init();

  loadAuth();

  // Setup WiFi via WiFiManager portal (open AP named "<serial>_setup")
  WiFiManager wm;
  wm.setDebugOutput(true);
  String apName = deviceSerial + String("_setup");
  showCentered("WiFi Setup", apName);
  // Try auto-connect using stored credentials. This blocks until connected or times out.
  bool connected = wm.autoConnect(apName.c_str(), "");
  if (!connected) {
    showCentered("WiFi Failed", "Restarting");
    delay(1500);
    ESP.restart();
  }

  Serial.println("WiFi connected: " + WiFi.localIP().toString());
  showCentered("WiFi connected", WiFi.localIP().toString());
  delay(800);

  // Attempt device login (or refresh)
  bool loggedIn = false;
  int attempts = 0;
  while (!loggedIn && attempts < 5) {
    attempts++;
    Serial.println("Login attempt #" + String(attempts));
    if (deviceLogin()) {
      loggedIn = true;
      break;
    }
    delay(2000);
  }

  if (!loggedIn) {
    showCentered("Login Failed", "Check Console");
    Serial.println("❌ FATAL: Could not authenticate");
    // Keep the device alive but not functional. Admin should fix via portal/DB.
    while (true) {
      delay(10000);
    }
  }

  showCentered("Master Ready", deviceSerial);
  delay(400);
}

// ================== LOOP ========================

void loop() {
  unsigned long now = millis();

  // battery refresh
  if (now - lastBatteryMs > BATTERY_INTERVAL_MS) {
    lastBatteryMs = now;
    float v = readBatteryVoltage();
    int pct = batteryPercent(v);
    // draw battery on current screen's right side
    drawBatteryIcon(pct);
    // also print to serial occasionally
    Serial.println("Battery: " + String(v, 2) + "V (" + String(pct) + "%)");
  }

  // heartbeat
  if (now - lastHeartbeatMs > HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMs = now;
    sendHeartbeat();
  }

  // commands polling
  if (now - lastCommandPollMs > COMMANDS_INTERVAL_MS) {
    lastCommandPollMs = now;
    pollCommands();
  }

  // RFID scanning (only if not in wifiConfigMode)
  if (wifiConfigMode) {
    delay(50);
    return;
  }

  if (!rfid.PICC_IsNewCardPresent()) {
    delay(10);
    return;
  }
  if (!rfid.PICC_ReadCardSerial()) {
    delay(10);
    return;
  }

  String uid = uidHexFromMFRC();
  Serial.println("Scanning UID: " + uid);

  // send and handle result
  processScannedUID(uid);

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  // small loop delay
  delay(50);
}
