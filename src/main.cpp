/*
  Master Device Firmware (WiFiManager + Cloud Commands)

  Features:
  - WiFi provisioning via WiFiManager (captive portal, no hard-coded SSID/PASS)
  - Device login to backend (/api/device/login) -> device_id, company_id, jwt, jwt_exp
  - Polls backend for commands (/api/master/commands?device_id=...)
  - Handles commands:
      - start_rfid_scan: read RFID, query Supabase customers, show name/balance or "New Tag" and notify backend
      - show_info: show company/device on LCD
      - force_wifi_setup: clear WiFi credentials and reboot into config portal
      - pair_slave: TODO
  - Sends heartbeat to /api/device/heartbeat
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>   // WiFiManager library
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>

// ------------------- PIN CONFIG -------------------
#define SS_PIN   5   // RFID SDA
#define RST_PIN  27   // RFID RST

// ------------------- RFID & LCD -------------------
MFRC522 rfid(SS_PIN, RST_PIN);
LiquidCrystal_I2C lcd(0x27, 16, 2); // change to 0x3F if needed

// ------------------- WIFI / DEVICE CONFIG -------------------
WiFiManager wm;

// Each master has a unique serial you assign when you sell it
String MASTER_SERIAL = "PMT-MASTER-0001";  // TODO: change per device

// Backend base URL (Lovable backend, NOT Supabase direct)
String API_BASE = "https://djodwbvntdlamhydpuih.supabase.co";  // TODO: change

// Supabase project URL (for direct REST customer lookups)
String SUPABASE_URL = "https://djodwbvntdlamhydpuih.supabase.co";
String SUPABASE_REST = SUPABASE_URL + String("/rest/v1");

// Safe to store on device (public)
String SUPABASE_ANON_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImRqb2R3YnZudGRsYW1oeWRwdWloIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjQ3MTQ1NTcsImV4cCI6MjA4MDI5MDU1N30.CfCR0UnTusBuc-_Pcaaycd6xyze9lTusEoxEj4hG0rg";  // TODO: change

// ------------------- STATE STORAGE -------------------
Preferences prefs;
String deviceId  = "";
String companyId = "";
String deviceJWT = "";
unsigned long jwtExpiry = 0;  // epoch seconds

unsigned long lastCommandPoll = 0;
unsigned long lastHeartbeat   = 0;

// ------------------- UTILITIES -------------------

unsigned long nowSeconds() {
  return millis() / 1000;
}

void showLCD(const String &line1, const String &line2 = "") {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1.substring(0, 16));
  lcd.setCursor(0, 1);
  lcd.print(line2.substring(0, 16));
}

// Generic HTTPS POST returning body
String httpPostJSON(String url, String jsonBody, int &httpCodeOut) {
  HTTPClient http;
  WiFiClientSecure *client = new WiFiClientSecure;
  client->setInsecure(); // TODO: use real CA cert in production

  http.begin(*client, url);
  http.addHeader("Content-Type", "application/json");

  Serial.println("POST " + url);
  Serial.println("Body: " + jsonBody);

  httpCodeOut = http.POST(jsonBody);
  String payload = http.getString();

  Serial.print("HTTP ");
  Serial.println(httpCodeOut);
  Serial.println(payload);

  http.end();
  delete client;

  return payload;
}

// Generic HTTPS GET returning body (with up to 3 headers)
String httpGetWithHeaders(String url,
                          const String &h1k, const String &h1v,
                          const String &h2k = "", const String &h2v = "",
                          int *httpCodeOut = nullptr) {
  HTTPClient http;
  WiFiClientSecure *client = new WiFiClientSecure;
  client->setInsecure();

  http.begin(*client, url);
  if (h1k.length() > 0) http.addHeader(h1k, h1v);
  if (h2k.length() > 0) http.addHeader(h2k, h2v);

  Serial.println("GET " + url);

  int code = http.GET();
  String payload = http.getString();

  Serial.print("HTTP ");
  Serial.println(code);
  Serial.println(payload);

  http.end();
  delete client;

  if (httpCodeOut) *httpCodeOut = code;
  return payload;
}

// ------------------- WIFI WITH WIFIMANAGER -------------------

String getConfigPortalName() {
  return MASTER_SERIAL + "-Setup";
}

void setupWiFi() {
  WiFi.mode(WIFI_STA);

  // Optional: auto-close portal after N seconds (otherwise stays until configured)
  wm.setConfigPortalTimeout(300); // 5 minutes

  // Called when portal starts
  wm.setAPCallback([](WiFiManager *wmPtr) {
    String ssid = wmPtr->getConfigPortalSSID();
    Serial.println("Config Portal started: " + ssid);
    // We can't call showLCD directly from lambda easily, so just log here.
    // On your main code you can show instructions like:
    // "Connect to: <serial>-Setup" on the LCD at boot.
  });

  String apName = getConfigPortalName();
  showLCD("WiFi: connecting", "");
  Serial.println("Starting WiFi autoConnect with AP: " + apName);

  bool res = wm.autoConnect(apName.c_str());  // blocks until connected or timeout

  if (!res) {
    Serial.println("WiFiManager: failed or timed out");
    showLCD("WiFi failed", "Config needed");
    // Decide what to do here:
    // - You can reboot and re-open portal
    // - Or remain in this state until power cycle
  } else {
    Serial.println("WiFi connected: " + WiFi.localIP().toString());
    showLCD("WiFi OK", WiFi.localIP().toString());
    delay(1000);
  }
}

// ------------------- SAVED DEVICE STATE -------------------

void loadState() {
  prefs.begin("masterdev", true); // read-only
  deviceId  = prefs.getString("device_id", "");
  companyId = prefs.getString("company_id", "");
  deviceJWT = prefs.getString("jwt", "");
  jwtExpiry = prefs.getULong("jwt_exp", 0);
  prefs.end();

  Serial.println("Loaded from NVS:");
  Serial.println("deviceId: " + deviceId);
  Serial.println("companyId: " + companyId);
  Serial.println("jwt: " + (deviceJWT.length() > 0 ? String("present") : String("empty")));
  Serial.println("jwtExpiry: " + String(jwtExpiry));
}

void saveState() {
  prefs.begin("masterdev", false);
  prefs.putString("device_id", deviceId);
  prefs.putString("company_id", companyId);
  prefs.putString("jwt", deviceJWT);
  prefs.putULong("jwt_exp", jwtExpiry);
  prefs.end();
}

// ------------------- DEVICE LOGIN / JWT -------------------

bool deviceLogin() {
  if (WiFi.status() != WL_CONNECTED) {
    setupWiFi();
    if (WiFi.status() != WL_CONNECTED) {
      showLCD("No WiFi", "Login failed");
      return false;
    }
  }

  String url = API_BASE + "/api/device/login"; // TODO: must exist on backend
  String body = "{";
  body += "\"device_serial\":\"" + MASTER_SERIAL + "\",";
  body += "\"device_type\":\"master\"";
  body += "}";

  int code;
  String resp = httpPostJSON(url, body, code);
  if (code != 200) {
    showLCD("Login failed", "HTTP " + String(code));
    return false;
  }

  // Expect JSON like:
  // { "device_id":"...", "company_id":"...", "jwt":"...", "jwt_exp": 1700000000 }
  int didPos = resp.indexOf("\"device_id\":\"");
  int cidPos = resp.indexOf("\"company_id\":\"");
  int jwtPos = resp.indexOf("\"jwt\":\"");
  int expPos = resp.indexOf("\"jwt_exp\":");

  if (didPos < 0 || cidPos < 0 || jwtPos < 0 || expPos < 0) {
    showLCD("Bad login resp", "");
    Serial.println("Parsing login response failed");
    return false;
  }

  didPos += 13;
  int didEnd = resp.indexOf("\"", didPos);
  deviceId = resp.substring(didPos, didEnd);

  cidPos += 14;
  int cidEnd = resp.indexOf("\"", cidPos);
  companyId = resp.substring(cidPos, cidEnd);

  jwtPos += 7;
  int jwtEnd = resp.indexOf("\"", jwtPos);
  deviceJWT = resp.substring(jwtPos, jwtEnd);

  expPos += 10;
  int expEnd = resp.indexOf("}", expPos);
  String expStr = resp.substring(expPos, expEnd);
  jwtExpiry = expStr.toInt();

  saveState();
  showLCD("Device login OK", "");
  Serial.println("deviceId=" + deviceId);
  Serial.println("companyId=" + companyId);
  Serial.println("jwtExpiry=" + String(jwtExpiry));
  delay(1000);
  return true;
}

bool ensureJWT() {
  if (deviceJWT.length() == 0 || nowSeconds() > jwtExpiry) {
    Serial.println("JWT missing or expired. Logging in...");
    return deviceLogin();
  }
  return true;
}

// ------------------- RFID HELPERS -------------------

bool readRFIDUID(String &uidOut) {
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    return false;
  }

  uidOut = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    uidOut += String(rfid.uid.uidByte[i], HEX);
  }
  uidOut.toUpperCase();

  rfid.PICC_HaltA();
  return true;
}

// Check customer on Supabase
void checkCustomerByRFID(const String &rfidUID) {
  if (!ensureJWT()) return;
  if (WiFi.status() != WL_CONNECTED) {
    showLCD("No WiFi", "Cust lookup");
    return;
  }

  String url = SUPABASE_REST + "/customers?select=name,balance&rfid_uid=eq." + rfidUID;

  int code;
  String resp = httpGetWithHeaders(
                  url,
                  "apikey", SUPABASE_ANON_KEY,
                  "Authorization", "Bearer " + deviceJWT,
                  &code
                );

  if (code != 200) {
    showLCD("Cust lookup err", "HTTP " + String(code));
    return;
  }

  if (resp == "[]" || resp.length() < 5) {
    // Not registered
    showLCD("New Tag", rfidUID);

    // Notify backend so web UI can pre-fill registration
    String body = "{";
    body += "\"rfid_uid\":\"" + rfidUID + "\",";
    body += "\"master_device_id\":\"" + deviceId + "\"";
    body += "}";

    int pcode;
    String url2 = API_BASE + "/api/master/new-rfid"; // TODO: implement backend
    httpPostJSON(url2, body, pcode);
    return;
  }

  // crude JSON parse: [{"name":"X","balance":123}]
  int namePos = resp.indexOf("\"name\":\"");
  if (namePos >= 0) {
    namePos += 8;
    int nameEnd = resp.indexOf("\"", namePos);
    String name = resp.substring(namePos, nameEnd);

    int balPos = resp.indexOf("\"balance\":");
    String balStr = "";
    if (balPos >= 0) {
      balPos += 10;
      int balEnd = resp.indexOf("}", balPos);
      balStr = resp.substring(balPos, balEnd);
    }

    showLCD(name, "Bal: " + balStr);
  } else {
    showLCD("Parse error", "");
  }
}

// ------------------- COMMAND HANDLING -------------------

void handleCommand(const String &action, const String &payload) {
  Serial.println("Handling cmd: " + action);

  if (action == "start_rfid_scan") {
    showLCD("Scan card...", "");
    String uid;
    unsigned long start = millis();
    while (millis() - start < 15000) { // wait up to 15 seconds
      if (readRFIDUID(uid)) {
        Serial.println("Scanned UID: " + uid);
        checkCustomerByRFID(uid);
        break;
      }
      delay(50);
    }
  } else if (action == "show_info") {
    showLCD("Company:", companyId);
    delay(2000);
    showLCD("Device:", deviceId);
    delay(2000);
  } else if (action == "force_wifi_setup") {
    Serial.println("Forcing WiFi setup...");
    showLCD("WiFi reset", "Rebooting...");
    delay(1000);
    // Clear WiFi settings then restart: next boot goes into portal
    wm.resetSettings();
    delay(500);
    ESP.restart();
  } else if (action == "pair_slave") {
    // TODO: implement AP mode + slave pairing handshake
    showLCD("Pair slave", "TODO");
    delay(1500);
  } else {
    showLCD("Unknown cmd", action);
    delay(1500);
  }
}

// Poll backend for commands
void pollCommands() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!ensureJWT()) return;

  String url = API_BASE + "/api/master/commands?device_id=" + deviceId; // TODO: implement backend
  int code;
  String resp = httpGetWithHeaders(url, "Authorization", "Bearer " + deviceJWT, "", "", &code);

  if (code != 200) {
    Serial.println("Cmd poll failed, HTTP " + String(code));
    return;
  }

  // Expect JSON like:
  // [ { "id": "...", "action": "start_rfid_scan", "payload": "{}" }, ... ]
  if (resp == "[]" || resp.length() < 5) return;

  int actPos = resp.indexOf("\"action\":\"");
  if (actPos < 0) return;
  actPos += 10;
  int actEnd = resp.indexOf("\"", actPos);
  String action = resp.substring(actPos, actEnd);

  String payload = "{}"; // TODO: parse if needed

  handleCommand(action, payload);

  // TODO: mark command as processed via POST /api/master/commands/ack
}

// Send heartbeat
void sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!ensureJWT()) return;

  String url = API_BASE + "/api/device/heartbeat"; // TODO: implement backend
  String body = "{";
  body += "\"device_id\":\"" + deviceId + "\",";
  body += "\"device_type\":\"master\",";
  body += "\"firmware_version\":\"1.0.0\"";
  body += "}";

  int code;
  httpPostJSON(url, body, code); // ignoring response for now
}

// ------------------- SETUP & LOOP -------------------

void setup() {
  Serial.begin(115200);
  delay(500);

  // LCD
  lcd.init();
  lcd.backlight();
  showLCD("Master Device", "Booting...");

  // RFID
  SPI.begin(18, 19, 23, SS_PIN);
  rfid.PCD_Init();

  // Load device state
  loadState();

  // WiFi provisioning (blocks until connected or timeout)
  setupWiFi();

  // Ensure we have deviceId/companyId/JWT
  if (deviceId.length() == 0 || companyId.length() == 0) {
    showLCD("Device login...", "");
    deviceLogin();
  } else {
    ensureJWT();
  }

  showLCD("Ready", "Waiting cmds");
}

void loop() {
  unsigned long now = millis();

  // If WiFi drops, try to reconnect in background
  if (WiFi.status() != WL_CONNECTED) {
    // Non-blocking reconnect attempt
    WiFi.reconnect();
  }

  // Poll commands every 3 seconds
  if (now - lastCommandPoll > 3000) {
    lastCommandPoll = now;
    pollCommands();
  }

  // Send heartbeat every 60 seconds
  if (now - lastHeartbeat > 60000) {
    lastHeartbeat = now;
    sendHeartbeat();
  }

  // Everything else is command-driven from dashboard
}
