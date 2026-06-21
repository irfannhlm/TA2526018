#include "PN532_Module.h"
#include "I2C_Handler.h"
#include <Wire.h>

Adafruit_PN532 nfc(-1, -1, &Wire); 
bool nfcActive = false; 
static unsigned long lastPn532InitAttempt = 0;

void initPN532() {
  lastPn532InitAttempt = millis();
  nfcActive = false;

  if (!lockI2C(150)) {
    return;
  }

  nfc.begin();

  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    nfcActive = false; 
    unlockI2C();
    return;
  }
  
  if (!nfc.SAMConfig()) {
    nfcActive = false;
    unlockI2C();
    return;
  }
  
  nfcActive = true; 
  unlockI2C();
}

bool ensurePN532Ready(uint32_t minRetryMs) {
  if (nfcActive) return true;

  unsigned long now = millis();
  if (minRetryMs > 0 &&
      lastPn532InitAttempt > 0 &&
      now - lastPn532InitAttempt < minRetryMs) {
    return false;
  }

  initPN532();
  return nfcActive;
}

String scanUID() {
  if (!ensurePN532Ready()) return "";

  if (!lockI2C(150)) return "";

  uint8_t success = 0;
  uint8_t uid[7];
  uint8_t uidLength = 0;

  success = nfc.readPassiveTargetID(
    PN532_MIFARE_ISO14443A,
    uid,
    &uidLength,
    100
  );

  unlockI2C();

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
