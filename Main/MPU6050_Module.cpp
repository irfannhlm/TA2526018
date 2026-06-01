#include "MPU6050_Module.h"
#include "I2C_Handler.h"
#include <math.h>

Adafruit_MPU6050 mpu;

volatile bool throwDetected = false;  // Dibaca oleh loop rekam

static TaskHandle_t throwTaskHandle = nullptr;
static volatile bool taskShouldRun = false;

void initMPU6050() {
  if (!mpu.begin()) {
    Serial.println("MPU6050 Gagal");
    return;
  }

  // Range 8G agar lebih toleran terhadap guncangan saat dilempar/ditangkap
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

  Serial.println("MPU6050 Ready");
}

static void throwDetectionTask(void* param) {
  bool isFlying = false;
  unsigned long flyStartTime = 0;

  const uint32_t SAMPLE_INTERVAL_MS = 10;      // 100 Hz
  const float FLYING_EXIT_THRESHOLD = 0.55f;   // hysteresis keluar flying

  while (taskShouldRun) {
    sensors_event_t a, g, temp;
    bool readOK = false;

    // WAJIB mutex karena mpu.getEvent() akses I2C
    if (lockI2C(5)) {
      mpu.getEvent(&a, &g, &temp);
      unlockI2C();
      readOK = true;
    }

    if (readOK) {
      float ax = a.acceleration.x / 9.81f;
      float ay = a.acceleration.y / 9.81f;
      float az = a.acceleration.z / 9.81f;
      float totalG = sqrtf(ax * ax + ay * ay + az * az);

      unsigned long now = millis();

      // Abaikan bacaan aneh
      if (!isnan(totalG) && totalG >= 0.02f && totalG <= 8.5f) {

        if (!isFlying) {
          // Masuk kandidat flying saat totalG rendah
          if (totalG < FLYING_THRESHOLD) {
            isFlying = true;
            flyStartTime = now;
          }
        } 
        else {
          // Hysteresis: jangan langsung batal kalau totalG naik sedikit
          if (totalG > FLYING_EXIT_THRESHOLD) {
            isFlying = false;
            flyStartTime = 0;
          } 
          else {
            if (now - flyStartTime >= FLYING_DURATION_MS) {
              throwDetected = true;
              taskShouldRun = false;
            }
          }
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(SAMPLE_INTERVAL_MS));
  }

  throwTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

void startThrowDetectionTask() {
  if (throwTaskHandle != nullptr) return;

  throwDetected = false;
  taskShouldRun = true;

  BaseType_t result = xTaskCreatePinnedToCore(
    throwDetectionTask,
    "ThrowDetect",
    3072,
    nullptr,
    2,
    &throwTaskHandle,
    0
  );

  if (result != pdPASS) {
    Serial.println("[MPU] Gagal membuat task ThrowDetect");
    throwTaskHandle = nullptr;
    taskShouldRun = false;
  }
}

void stopThrowDetectionTask() {
  taskShouldRun = false;

  unsigned long startWait = millis();
  while (throwTaskHandle != nullptr && millis() - startWait < 100) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void resetFlyingFlag() {
  throwDetected = false;
}