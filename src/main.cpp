#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ---------------- LCD -----------------
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ---------------- RFID -----------------
#define SS_PIN 21     // SDA
#define RST_PIN 22     // RST (you said pin 1 is OK too)
MFRC522 rfid(SS_PIN, RST_PIN);

unsigned long lastReadTime = 0;
bool tagDisplayed = false;

// ---------------- Battery -----------------
#define BATT_PIN 34  // ADC pin

float R1 = 2200.0;  // top resistor (2.2k)
float R2 = 2200.0;  // bottom resistor (2.2k)
float VREF = 3.3;   // ESP32 ADC reference

// Custom battery bar characters
byte bar0[8] = {B00000,B00000,B00000,B00000,B00000,B00000,B00000,B00000};
byte bar1[8] = {B10000,B10000,B10000,B10000,B10000,B10000,B10000,B10000};
byte bar2[8] = {B11000,B11000,B11000,B11000,B11000,B11000,B11000,B11000};
byte bar3[8] = {B11100,B11100,B11100,B11100,B11100,B11100,B11100,B11100};
byte bar4[8] = {B11110,B11110,B11110,B11110,B11110,B11110,B11110,B11110};
byte bar5[8] = {B11111,B11111,B11111,B11111,B11111,B11111,B11111,B11111};

// Squeezed percentage characters 0%–9%
byte d0p[8] = {B01110,B10001,B10001,B10001,B01110,B00100,B01010,B00100};
byte d1p[8] = {B00100,B01100,B00100,B00100,B00100,B00100,B01010,B00100};
byte d2p[8] = {B01110,B10001,B00010,B00100,B01000,B00100,B01010,B00100};
byte d3p[8] = {B01110,B10001,B00010,B00110,B10001,B01110,B01010,B00100};
byte d4p[8] = {B00010,B00110,B01010,B10010,B11111,B00100,B01010,B00100};
byte d5p[8] = {B11111,B10000,B11110,B00001,B10001,B01110,B01010,B00100};
byte d6p[8] = {B00110,B01000,B10000,B11110,B10001,B01110,B01010,B00100};
byte d7p[8] = {B11111,B00001,B00010,B00100,B01000,B10000,B01010,B00100};
byte d8p[8] = {B01110,B10001,B01110,B10001,B01110,B00100,B01010,B00100};
byte d9p[8] = {B01110,B10001,B10001,B01111,B00001,B00110,B01010,B00100};

byte* tinyPercent[10] = {d0p,d1p,d2p,d3p,d4p,d5p,d6p,d7p,d8p,d9p};
int lastDigitLoaded = -1;

// ---------------- Setup -----------------
void setup() {
  Serial.begin(115200);
  SPI.begin(18, 19, 23, SS_PIN); // SCK, MISO, MOSI, SS
  rfid.PCD_Init();

  lcd.init();
  lcd.backlight();

  // Preload battery bars (5 chars)
  lcd.createChar(0, bar0);
  lcd.createChar(1, bar1);
  lcd.createChar(2, bar2);
  lcd.createChar(3, bar3);
  lcd.createChar(4, bar4);
  lcd.createChar(5, bar5);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("RFID Ready");
}

// ---------------- Battery Functions -----------------
int getBatteryPercent() {
  int raw = analogRead(BATT_PIN);
  float voltage = (raw / 4095.0) * VREF;
  float batt = voltage * ((R1 + R2) / R2);
  int percent = map(batt * 100, 330, 420, 0, 100);
  percent = constrain(percent, 0, 100);
  return percent;
}

void drawBatteryBar(int percent) {
  int segments = map(percent, 0, 100, 0, 5);
  lcd.setCursor(15, 0);
  lcd.write(segments); // draw loading bar char
}

void printPercent(int percent) {
  lcd.setCursor(13, 1);

  if (percent == 100) {
    lcd.print("100");
    return;
  }

  int tens = percent / 10;
  int ones = percent % 10;

  lcd.print(tens);

  lcd.setCursor(14, 1);

  if (ones != lastDigitLoaded) {
    lcd.createChar(7, tinyPercent[ones]); // overwrite slot 7
    lastDigitLoaded = ones;
  }

  lcd.write(7);
}

void blinkLowBattery(int percent) {
  if (percent <= 20 && (millis()/500)%2==0) {
    lcd.setCursor(15, 0);
    lcd.print(" ");  // hide bar
  }
}

// ---------------- RFID Section -----------------
void displayUID(MFRC522::Uid uid) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("TAG READ!");

  lcd.setCursor(0, 1);

  for (byte i = 0; i < uid.size; i++) {
    if (uid.uidByte[i] < 0x10) lcd.print("0");
    lcd.print(uid.uidByte[i], HEX);
  }

  lastReadTime = millis();
  tagDisplayed = true;
}

void clearTagAfter10s() {
  if (tagDisplayed && millis() - lastReadTime >= 10000) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Ready for Tag");
    tagDisplayed = false;
  }
}

// ---------------- Loop -----------------
void loop() {

  // ---------------- RFID ---------------
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    displayUID(rfid.uid);
    rfid.PICC_HaltA();
  }

  clearTagAfter10s();

  // ---------------- Battery -------------
  int percent = getBatteryPercent();
  drawBatteryBar(percent);
  blinkLowBattery(percent);
  printPercent(percent);
}
