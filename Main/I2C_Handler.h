#pragma once

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

extern SemaphoreHandle_t i2cMutex;

void initI2CMutex();
bool lockI2C(uint32_t timeoutMs = 50);
void unlockI2C();