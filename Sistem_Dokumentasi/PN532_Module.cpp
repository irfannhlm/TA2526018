#include "PN532_Module.h"
#include <Wire.h>

Adafruit_PN532 nfc(-1, -1, &Wire); 

void initPN532() {
  // Wire.begin sudah di file .ino utama
  nfc.begin();

  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("PN532 gagal");
    return;
  }
  
  Serial.println("PN532 Ready");
  nfc.SAMConfig();
}

String scanUID() {
  uint8_t success;
  uint8_t uid[7];
  uint8_t uidLength;

  // Scan kartu dengan timeout 100ms
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

