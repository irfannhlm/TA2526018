#include "Buzzer_Module.h"
#include "Config.h"

struct BuzzerState {
  bool active = false;
  bool isOn = false;

  uint8_t remainingBeeps = 0;
  uint16_t onMs = 0;
  uint16_t offMs = 0;

  unsigned long lastChange = 0;
};

static BuzzerState buzzer;

static void buzzerToneOn() {
  tone(BUZZER_PIN, BUZZER_TONE_HZ);
}

static void buzzerToneOff() {
  noTone(BUZZER_PIN);
  digitalWrite(BUZZER_PIN, LOW);
}

void initBuzzer() {
  pinMode(BUZZER_PIN, OUTPUT);
  buzzerToneOff();
}

void playBuzzer(uint8_t count, uint16_t onMs, uint16_t offMs) {
  if (count == 0 || onMs == 0) {
    stopBuzzer();
    return;
  }

  buzzer.active = true;
  buzzer.isOn = true;
  buzzer.remainingBeeps = count;
  buzzer.onMs = onMs;
  buzzer.offMs = offMs;
  buzzer.lastChange = millis();

  buzzerToneOn();
}

void updateBuzzer() {
  if (!buzzer.active) return;

  unsigned long now = millis();

  if (buzzer.isOn) {
    if (now - buzzer.lastChange >= buzzer.onMs) {
      buzzerToneOff();
      buzzer.isOn = false;
      buzzer.lastChange = now;

      if (buzzer.remainingBeeps > 0) {
        buzzer.remainingBeeps--;
      }

      if (buzzer.remainingBeeps == 0) {
        buzzer.active = false;
      }
    }
  } else {
    if (now - buzzer.lastChange >= buzzer.offMs) {
      buzzerToneOn();
      buzzer.isOn = true;
      buzzer.lastChange = now;
    }
  }
}

void stopBuzzer() {
  buzzer.active = false;
  buzzer.isOn = false;
  buzzer.remainingBeeps = 0;
  buzzerToneOff();
}

void waitBuzzerDone() {
  while (isBuzzerBusy()) {
    updateBuzzer();
    delay(5);
  }
}

bool isBuzzerBusy() {
  return buzzer.active;
}
