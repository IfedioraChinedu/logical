/*******************************************************
 * MASTER DEVICE FIRMWARE (Option A - Strict)
 * - Uses tzapu WiFiManager (captive portal)
 * - Auto WiFi setup page with footer hint
 * - Device MUST login to backend or it will not run RFID
 * - RFID UID sent to backend /api/master/new-rfid
 * - Heartbeat + command polling (start_wifi_manager)
 *******************************************************/

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiManager.h>     // tzapu WiFiManager
#include <SPI.h>
#include <MFRC522.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>

// ================== RFID PINS ==================
#define RFID_SS_PIN   5
#define RFID_RST_PIN  27
#define RFID_SCK_PIN  18
#define RFID_MOSI_PIN 23
#define RFID_MISO_PIN 19

// ============== BATTERY PIN ====================
#define BATTERY_PIN   34

// ================= LCD =========================
LiquidCrystal_I2C lcd(0x27, 16, 2);  // change 0x27 to 0x3F if needed

// Battery icons
byte battery0[8] = {B01110,B10001,B10001,B10001,B10001,B10001,B10001,B11111};
byte battery1[8] = {B01110,B10001,B10001,B10001,B10001,B10001,B11111,B11111};
byte battery2[8] = {B01110,B10001,B10001,B10001,B11111,B11111,B11111,B11111};
byte battery3[8] = {B01110,B10001,B10001,B11111,B11111,B11111,B11111,B11111};
byte battery4[8] = {B01110,B11111,B11111,B11111,B11111,B11111,B11111,B11111};

// =============== BACKEND CONFIG ===============
// TODO: CHANGE THIS to your Lovable backend
const char* API_BASE = "https://your-backend-url.com";

const char* DEVICE_LOGIN_PATH   = "/api/device/login";
const char* HEARTBEAT_PATH      = "/api/device/heartbeat";
const char* NEW_RFID_PATH       = "/api/master/new-rfid";
const char* COMMANDS_PATH       = "/api/master/commands";
const char* COMMANDS_ACK_PATH   = "/api/master/commands/ack";

// =============== DEVICE IDENTITY ===============
Preferences prefs;

// TODO: CHANGE THIS per device (must match what you register in DB)
String deviceSerial = "MASTER_001";

String deviceId  = "";
String companyId = "";
String deviceJWT = "";

bool isLoggedIn = false;   // strict mode: must be true before RFID/commands

// =============== RFID =========================
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
String lastSentUID = "";

// =============== TIMERS ========================
unsigned long lastHeartbeatMs   = 0;
unsigned long lastBatteryMs     = 0;
unsigned long lastCommandPollMs = 0;

const unsigned long HEARTBEAT_INTERVAL_MS = 60000;
const unsigned long BATTERY_INTERVAL_MS   = 5000;
const unsigned long COMMANDS_INTERVAL_MS  = 5000;

// =============== UTILS ========================

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

  int x1 = (int)(16 - a.length()) / 2;
  if (x1 < 0) x1 = 0;
  lcd.setCursor(x1, 0);
  lcd.print(a);

  if (b.length()) {
    int x2 = (int)(16 - b.length()) / 2;
    if (x2 < 0) x2 = 0;
    lcd.setCursor(x2, 1);
    lcd.print(b);
  }
}

// ================= BATTERY =====================

bool blinkState = false;

float readBatteryVoltage() {
  int raw = analogRead(BATTERY_PIN);
  float v_adc = raw * 3.3f / 4095.0f;
  return v_adc * 2.0f; // 2:1 divider
}

int batteryPercent(float vbat) {
  const float VMIN = 3.3;
  const float VMAX = 4.2;

  if (vbat <= VMIN) return 0;
  if (vbat >= VMAX) return 100;
  return (vbat - VMIN) * 100 / (VMAX - VMIN);
}

void updateBatteryIcon() {
  float v = readBatteryVoltage();
  int pct = batteryPercent(v);

  byte icon;
  if (pct <= 5)       icon = 0;
  else if (pct <= 25) icon = 1;
  else if (pct <= 50) icon = 2;
  else if (pct <= 75) icon = 3;
  else                icon = 4;

  lcd.setCursor(15, 0);

  if (pct < 20) {
    blinkState = !blinkState;
    if (blinkState) lcd.write(icon);
    else lcd.print(" ");
  } else {
    lcd.write(icon);
  }
}

// =========== LOAD AUTH =========================
void loadAuth() {
  deviceId  = prefs.getString("deviceId", "");
  companyId = prefs.getString("companyId", "");
  deviceJWT = prefs.getString("deviceJWT", "");

  Serial.println("Loaded from NVS:");
  Serial.println("deviceId: " + deviceId);
  Serial.println("companyId: " + companyId);
  Serial.println("jwt present: " + String(deviceJWT.length() > 0));
}

// =============== HTTP HELPERS ==================

bool httpPostJson(const char* path, String body, String &response, bool auth = true) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("HTTP POST: WiFi not connected");
    return false;
  }

  HTTPClient http;
  String url = String(API_BASE) + path;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  if (auth && deviceJWT.length())
    http.addHeader("Authorization", "Bearer " + deviceJWT);

  Serial.println("POST " + url);
  Serial.println("Body: " + body);

  int code = http.POST(body);
  if (code <= 0) {
    Serial.println("HTTP POST failed, code: " + String(code));
    http.end();
    return false;
  }

  response = http.getString();
  Serial.println("HTTP " + String(code));
  Serial.println(response);

  http.end();
  return (code >= 200 && code < 300);
}

bool httpGet(String url, String &response) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("HTTP GET: WiFi not connected");
    return false;
  }

  HTTPClient http;
  http.begin(url);

  if (deviceJWT.length())
    http.addHeader("Authorization", "Bearer " + deviceJWT);

  Serial.println("GET " + url);

  int code = http.GET();
  if (code <= 0) {
    Serial.println("HTTP GET failed, code: " + String(code));
    http.end();
    return false;
  }

  response = http.getString();
  Serial.println("HTTP " + String(code));
  Serial.println(response);

  http.end();
  return (code >= 200 && code < 300);
}

// ================= WIFI (WiFiManager) ===================

void setupWiFi() {
  WiFiManager wm;

  // Force config portal if cannot connect
  wm.setDebugOutput(true);

  // Make AP stable & discoverable
  wm.setAPStaticIPConfig(
    IPAddress(192,168,4,1),
    IPAddress(192,168,4,1),
    IPAddress(255,255,255,0)
  );

  // 5 minutes timeout for portal
  wm.setTimeout(300);

  // Called when config portal starts
  wm.setAPCallback([](WiFiManager *wmPtr) {
    Serial.println("CONFIG PORTAL ACTIVE!");
  });

  // Add footer-like text (note: shown at top; WiFiManager injects this into the page)
  wm.setCustomHeadElement(
    "<div style='text-align:center;font-size:13px;margin-top:10px;'>"
    "Or connect manually: <b>http://192.168.4.1</b>"
    "</div>"
  );
  // NOTE: Use http://, not https:// — ESP32 does not host HTTPS

  showCentered("WiFi Setup...", "");

  // AP SSID: MASTER_001_Setup etc
  String apName = deviceSerial + "_Setup";

  bool res = wm.autoConnect(apName.c_str(), "12345678");

  if (!res) {
    Serial.println("WiFiManager: failed or timed out");
    showCentered("WiFi Failed", "Restart needed");
    // strict: cannot proceed without WiFi
    while (true) {
      delay(1000);
    }
  } else {
    Serial.print("WiFi connected: ");
    Serial.println(WiFi.localIP());
    showCentered("WiFi OK", WiFi.localIP().toString());
    delay(1000);
  }
}

// ================= LOGIN ========================

bool deviceLogin() {
  String body =
    "{\"device_serial\":\"" + deviceSerial +
    "\",\"device_type\":\"master\"}";

  String res;
  Serial.println("Attempting device login for serial: " + deviceSerial);

  if (!httpPostJson(DEVICE_LOGIN_PATH, body, res, false)) {
    Serial.println("HTTP ERROR: could not reach backend.");
    return false;
  }

  Serial.println("Login response: " + res);

  // Basic error detection (backend should send error field if not registered)
  if (res.indexOf("error") >= 0) {
    Serial.println("Backend error: " + res);
    return false;
  }

  deviceId  = getJsonString(res, "device_id");
  companyId = getJsonString(res, "company_id");
  deviceJWT = getJsonString(res, "jwt");

  if (deviceId == "" || companyId == "" || deviceJWT == "") {
    Serial.println("ERROR: Login response missing fields.");
    return false;
  }

  // Persist identity
  prefs.putString("deviceId", deviceId);
  prefs.putString("companyId", companyId);
  prefs.putString("deviceJWT", deviceJWT);

  Serial.println("Device login SUCCESS");
  Serial.println("deviceId: " + deviceId);
  Serial.println("companyId: " + companyId);
  Serial.println("JWT stored");

  return true;
}

// ================= HEARTBEAT =====================

void sendHeartbeat() {
  if (!isLoggedIn) return;
  if (WiFi.status() != WL_CONNECTED) return;

  String body =
    "{\"device_id\":\"" + deviceId +
    "\",\"device_type\":\"master\",\"firmware_version\":\"1.0.0\"}";

  String res;
  httpPostJson(HEARTBEAT_PATH, body, res, true);
}

// ================== COMMANDS ====================

void ackCommand(String id) {
  String body = "{\"command_id\":\"" + id + "\"}";
  String res;
  httpPostJson(COMMANDS_ACK_PATH, body, res, true);
}

void pollCommands() {
  if (!isLoggedIn) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (deviceId == "") return;

  String url = String(API_BASE) + COMMANDS_PATH + "?device_id=" + deviceId;

  String res;
  if (!httpGet(url, res)) return;

  // Look for "start_wifi_manager" command
  if (res.indexOf("start_wifi_manager") >= 0) {

    int idStart = res.indexOf("\"id\":\"") + 6;
    int idEnd   = res.indexOf("\"", idStart);
    String cmdId = res.substring(idStart, idEnd);

    ackCommand(cmdId);

    showCentered("WiFi Reset", "Rebooting");
    delay(600);

    WiFiManager wm;
    wm.resetSettings();   // clear stored WiFi creds
    delay(500);
    ESP.restart();
  }
}

// ================= RFID ==========================

String uidHex() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

void sendRFID(String uid) {
  if (!isLoggedIn) return;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("RFID ignored: no WiFi");
    return;
  }

  String body =
    "{\"rfid_uid\":\"" + uid +
    "\",\"master_device_id\":\"" + deviceId + "\"}";

  String res;
  if (httpPostJson(NEW_RFID_PATH, body, res, true)) {
    Serial.println("RFID UID sent: " + uid);
  } else {
    Serial.println("Failed to send RFID UID");
  }
}

// ================= SETUP =========================

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

  loadAuth();

  // 1) WiFi provisioning (blocking until connected)
  setupWiFi();

  // 2) Strict: must login to backend
  bool loggedIn = false;
  int attempts = 0;

  showCentered("Login Device", "");

  while (!loggedIn && attempts < 5) {
    attempts++;
    Serial.println("Login attempt #" + String(attempts));

    if (deviceLogin()) {
      loggedIn = true;
      isLoggedIn = true;
      showCentered("Login OK", "Master Ready");
      delay(1200);
    } else {
      showCentered("Login Failed", "Retrying...");
      delay(1500);
    }
  }

  if (!loggedIn) {
    showCentered("Login Failed", "Check Backend");
    Serial.println("FATAL: device could not authenticate. Stopping.");
    // strict: halt device
    while (true) {
      delay(1000);
    }
  }
}

// ================= LOOP =========================

void loop() {
  unsigned long now = millis();

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

  // Commands
  if (now - lastCommandPollMs > COMMANDS_INTERVAL_MS) {
    lastCommandPollMs = now;
    pollCommands();
  }

  // RFID only if logged in (strict mode)
  if (!isLoggedIn) return;
  if (WiFi.status() != WL_CONNECTED) {
    showCentered("No WiFi", "RFID blocked");
    delay(500);
    return;
  }

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