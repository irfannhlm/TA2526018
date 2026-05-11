#include "PN532_Module.h"
#include <Wire.h>

Adafruit_PN532 nfc(-1, -1, &Wire); 
bool nfcActive = false; 

// A helper function to flush the unread response bytes 
// so the PN532 doesn't jam the SCL line.
void flushI2CBuffer() {
  delay(10); // Give the PN532 time to prep its response
  Wire.requestFrom(0x24, 10); // Request up to 10 bytes
  while(Wire.available()) {
    Wire.read(); // Read and discard
  }
}

void initPN532() {
  nfc.begin();

  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("PN532 gagal ditemukan");
    nfcActive = false; 
    return;
  }

  Serial.println("PN532 Ready");
  nfc.SAMConfig();
  
  nfcActive = true; 
}

void powerDownPN532() {
  // Command 0x16: PowerDown command
  // Item 0x80: WakeUpEnable byte (0x80 for I2C)
  uint8_t powerDownCmd[] = {0x16, 0x80};
  nfc.sendCommandCheckAck(powerDownCmd, 3);
  flushI2CBuffer();
  delay(1); // wait 1ms for PN532 to turn off
}

String scanUID() {
  if (!nfcActive) return ""; // Kalau gagal, jangan paksa scan terus

  uint8_t success;
  uint8_t uid[7];
  uint8_t uidLength;

  // Timeout 100ms
  success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 100);

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