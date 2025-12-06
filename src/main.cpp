#include <WiFi.h>
#include <HTTPClient.h>
#include <MFRC522.h>
#include <SPI.h>
#include <LiquidCrystal_I2C.h>

#define SS_PIN 5 //sda
#define RST_PIN 27 //rst

MFRC522 rfid(SS_PIN, RST_PIN);
LiquidCrystal_I2C lcd(0x27, 16, 2);

const char* wifi = "Imaxeuno";
const char* pass = "89password";

String apiURL = "https://djodwbvntdlamhydpuih.supabase.co/rest/v1/customers?select=name,rfid_uid&rfid_uid=eq.";
String apikey = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImRqb2R3YnZudGRsYW1oeWRwdWloIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjQ3MTQ1NTcsImV4cCI6MjA4MDI5MDU1N30.CfCR0UnTusBuc-_Pcaaycd6xyze9lTusEoxEj4hG0rg";
String jwt = "eyJhbGciOiJIUzI1NiIsImtpZCI6Illha0gyaUpIZ2tOL2ViVlkiLCJ0eXAiOiJKV1QifQ.eyJpc3MiOiJodHRwczovL2Rqb2R3YnZudGRsYW1oeWRwdWloLnN1cGFiYXNlLmNvL2F1dGgvdjEiLCJzdWIiOiIwMDQwMTc0OS04YWUxLTQxMDctOTA3YS1jZmYwMWYzZDA3ZWIiLCJhdWQiOiJhdXRoZW50aWNhdGVkIiwiZXhwIjoxNzY0OTMyNjQ2LCJpYXQiOjE3NjQ5MjkwNDYsImVtYWlsIjoiY2hpbmVkdWlmZWRpb3JhZGF2aWRAZ21haWwuY29tIiwicGhvbmUiOiIiLCJhcHBfbWV0YWRhdGEiOnsicHJvdmlkZXIiOiJlbWFpbCIsInByb3ZpZGVycyI6WyJlbWFpbCJdfSwidXNlcl9tZXRhZGF0YSI6eyJlbWFpbCI6ImNoaW5lZHVpZmVkaW9yYWRhdmlkQGdtYWlsLmNvbSIsImVtYWlsX3ZlcmlmaWVkIjp0cnVlLCJmdWxsX25hbWUiOiJJZmVkaW9yYSIsInBob25lX3ZlcmlmaWVkIjpmYWxzZSwic3ViIjoiMDA0MDE3NDktOGFlMS00MTA3LTkwN2EtY2ZmMDFmM2QwN2ViIn0sInJvbGUiOiJhdXRoZW50aWNhdGVkIiwiYWFsIjoiYWFsMSIsImFtciI6W3sibWV0aG9kIjoicGFzc3dvcmQiLCJ0aW1lc3RhbXAiOjE3NjQ5MjkwNDZ9XSwic2Vzc2lvbl9pZCI6ImJlOGJjNzk4LTQzNDEtNDg3ZC1iMGMwLTQyMDJiYjM1ODI2ZCIsImlzX2Fub255bW91cyI6ZmFsc2V9.V3CXU_U8_XbL4hZZRep-7J49Z0cxOWDUXN7KL6ZXIfE";

void setup() {
  Serial.begin(115200);

  lcd.init();
  lcd.backlight();
  lcd.print("Booting...");

  SPI.begin(18, 19, 23, SS_PIN);
  rfid.PCD_Init();

  WiFi.begin(wifi, pass);
  while (WiFi.status() != WL_CONNECTED) delay(300);

  lcd.clear();
  lcd.print("WiFi OK");
  delay(500);
}

void loop() {
  lcd.clear();
  lcd.print("Scan tag...");

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++)
    uid += String(rfid.uid.uidByte[i], HEX);
  uid.toUpperCase();

  Serial.println("Scanning UID: " + uid);
  lcd.clear();
  lcd.print("Checking...");

  HTTPClient http;
  http.begin(apiURL + uid);

  http.addHeader("apikey", apikey);
  http.addHeader("Authorization", "Bearer " + jwt);

  int code = http.GET();

  Serial.println("HTTP: " + String(code));

  if (code == 200) {
    String body = http.getString();
    Serial.println(body);

    if (body.length() > 5) {
      int p1 = body.indexOf(":\"") + 2;
      int p2 = body.indexOf("\"", p1);
      String name = body.substring(p1, p2);

      lcd.clear();
      lcd.print("Hello ");
      lcd.print(name);
    } else {
      lcd.clear();
      lcd.print("Not registered");
    }
  } else {
    lcd.clear();
    lcd.print("Net error");
  }

  http.end();
  delay(2000);
}
