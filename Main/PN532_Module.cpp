#include "PN532_Module.h"
#include <Wire.h>

Adafruit_PN532 nfc(-1, -1, &Wire); 
bool nfcActive = false; 

void initPN532() {
  nfc.begin();

  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("PN532 failed");
    nfcActive = false; 
    return;
  }
  
  Serial.println("PN532 Ready");
  nfc.SAMConfig();
  
  nfcActive = true; 
}

void flushI2CBuffer() {
  delay(10);
  Wire.requestFrom(0x24, 10);
  while (Wire.available()) {
    Wire.read();
  }
}

void powerDownPN532() {
  uint8_t powerDownCmd[] = {0x16, 0x80};

  // Tadinya 3, tapi array cuma 2 byte
  nfc.sendCommandCheckAck(powerDownCmd, 2);

  flushI2CBuffer();
  delay(1);
}

String scanUID() {
  if (!nfcActive) return "";

  uint8_t success;
  uint8_t uid[7];
  uint8_t uidLength;

  success = nfc.readPassiveTargetID(
    PN532_MIFARE_ISO14443A,
    uid,
    &uidLength,
    100
  );

  if (success) {
    String currentUID = "";
    for (uint8_t i = 0; i < uidLength; i++) {
      if (uid[i] < 0x10) currentUID += "0";
      currentUID += String(uid[i], HEX);
    }
    currentUID.toUpperCase();
    return currentUID;
  }

  return ""; 
}

String scanUIDPeriodik(uint32_t intervalMs) {
  static unsigned long lastScanTime = 0;

  if (millis() - lastScanTime < intervalMs) {
    return "";
  }

  lastScanTime = millis();
  return scanUID();
}