#include "I2C_Handler.h"

SemaphoreHandle_t i2cMutex = nullptr;

void initI2CMutex() {
  if (i2cMutex == nullptr) {
    i2cMutex = xSemaphoreCreateMutex();
  }
}

bool lockI2C(uint32_t timeoutMs) {
  if (i2cMutex == nullptr) return true;
  return xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void unlockI2C() {
  if (i2cMutex != nullptr) {
    xSemaphoreGive(i2cMutex);
  }
}