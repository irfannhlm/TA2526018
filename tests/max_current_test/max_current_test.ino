#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_PN532.h>

// --- Pin Definitions (UPDATE THESE TO MATCH YOURS) ---
#define SDA_PIN 21
#define SCL_PIN 22
#define BUZZER_PIN 5
#define LED1_PIN 18 
#define LED2_PIN 19 

LiquidCrystal_I2C lcd(0x27, 16, 2);
#define PN532_IRQ   2
#define PN532_RESET 3
Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);

WiFiUDP udp;
const char* dummyData = "MAX_POWER_TRANSMISSION_STRESS_TEST_DATA_PACKET_";

void setup() {
  Serial.begin(115200);

  // 1. Actuators ON
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED1_PIN, OUTPUT);
  pinMode(LED2_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, HIGH);
  digitalWrite(LED1_PIN, HIGH);
  digitalWrite(LED2_PIN, HIGH);

  // 2. I2C & LCD
  Wire.begin(SDA_PIN, SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("STRESS TESTING");

  // 3. Setup PN532
  nfc.begin();
  nfc.SAMConfig(); 

  // 4. Force Max Power WiFi Transmission
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESP32_STRESS_TEST", "12345678");
  WiFi.setTxPower(WIFI_POWER_19_5dBm); // Hard-lock the amplifier to max power
  udp.begin(1234);
}

void loop() {
  // 1. Keep Actuators Pinned
  digitalWrite(BUZZER_PIN, HIGH);
  digitalWrite(LED1_PIN, HIGH);
  digitalWrite(LED2_PIN, HIGH);

  // 2. Poll NFC (50ms timeout)
  uint8_t success;
  uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };
  uint8_t uidLength;
  success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 50); 

  if (success) {
    lcd.setCursor(0, 1);
    for (uint8_t i = 0; i < uidLength; i++) {
      if (uid[i] <= 0x0F) {
        lcd.print("0");
      }
      lcd.print(uid[i], HEX);
    }
    lcd.print("      "); 
  } else {
    lcd.setCursor(0, 1);
    lcd.print("SCANNING...     "); 
  }

  // 3. Flood the network with massive UDP packets constantly
  udp.beginPacket("255.255.255.255", 1234); // Broadcast to everything
  for(int i = 0; i < 20; i++){
      udp.print(dummyData); // Stuff the packet full of bytes
  }
  udp.endPacket(); // This forces the radio to physically transmit
}