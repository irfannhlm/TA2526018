#pragma once

#include <Arduino.h>

void initBuzzer();
void updateBuzzer();
void playBuzzer(uint8_t count, uint16_t onMs, uint16_t offMs);
void stopBuzzer();
bool isBuzzerBusy();
void waitBuzzerDone();