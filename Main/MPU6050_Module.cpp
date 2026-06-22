#include "MPU6050_Module.h"
#include "I2C_Handler.h"
#include <math.h>

Adafruit_MPU6050 mpu;

volatile bool throwDetected = false;  // Dibaca oleh loop rekam

static bool mpuReady = false;
static TaskHandle_t throwTaskHandle = nullptr;
static volatile bool taskShouldRun = false;

enum ThrowDetectionState {
  THROW_STATE_IDLE,
  THROW_STATE_AIRBORNE_CANDIDATE,
  THROW_STATE_WAIT_IMPACT
};

static float gyroMagnitudeDps(const sensors_event_t &g) {
  const float RAD_TO_DPS = 57.2957795f;
  float gx = g.gyro.x * RAD_TO_DPS;
  float gy = g.gyro.y * RAD_TO_DPS;
  float gz = g.gyro.z * RAD_TO_DPS;

  return sqrtf(gx * gx + gy * gy + gz * gz);
}

void initMPU6050() {
  if (!mpu.begin()) {
    mpuReady = false;
    return;
  }

  // Range 8G agar lebih toleran terhadap guncangan saat dilempar/ditangkap
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setGyroRange(MPU6050_RANGE_2000_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_44_HZ);

  mpuReady = true;
}

static void throwDetectionTask(void* param) {
  ThrowDetectionState state = THROW_STATE_IDLE;
  unsigned long stateStartedAt = 0;
  unsigned long impactWindowStartedAt = 0;
  unsigned long lastValidSampleAt = 0;
  unsigned long lowGMs = 0;
  unsigned long spinMs = 0;
  unsigned long strongSpinMs = 0;
  float minTotalG = 99.0f;
  float peakGyroDps = 0.0f;
  float peakImpactG = 0.0f;
  float previousTotalG = 1.0f;

  while (taskShouldRun) {
    sensors_event_t a, g, temp;
    bool readOK = false;

    // mutex karena mpu.getEvent() akses I2C
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
      float gyroDps = gyroMagnitudeDps(g);

      unsigned long now = millis();

      // Abaikan bacaan aneh
      if (!isnan(totalG) && !isnan(gyroDps) && totalG >= 0.02f && totalG <= 8.5f && gyroDps <= 4000.0f) {
        unsigned long sampleDeltaMs = THROW_SAMPLE_INTERVAL_MS;
        if (lastValidSampleAt > 0) {
          sampleDeltaMs = now - lastValidSampleAt;
          if (sampleDeltaMs == 0 || sampleDeltaMs > 25UL) {
            sampleDeltaMs = THROW_SAMPLE_INTERVAL_MS;
          }
        }
        lastValidSampleAt = now;

        bool lowGEvidence = totalG < THROW_LOW_G_THRESHOLD;
        bool spinEvidence = gyroDps > THROW_SPIN_DPS && totalG < THROW_SPIN_MAX_G;
        bool strongSpinEvidence = gyroDps > THROW_STRONG_SPIN_DPS && totalG < THROW_STRONG_SPIN_MAX_G;
        bool airborneEvidence = lowGEvidence || spinEvidence || strongSpinEvidence;

        if (state != THROW_STATE_IDLE) {
          if (totalG < minTotalG) minTotalG = totalG;
          if (gyroDps > peakGyroDps) peakGyroDps = gyroDps;
          if (totalG > peakImpactG) peakImpactG = totalG;

          if (lowGEvidence) lowGMs += sampleDeltaMs;
          if (spinEvidence) spinMs += sampleDeltaMs;
          if (strongSpinEvidence) strongSpinMs += sampleDeltaMs;
        }

        switch (state) {
          case THROW_STATE_IDLE:
            if (airborneEvidence) {
              state = THROW_STATE_AIRBORNE_CANDIDATE;
              stateStartedAt = now;
              lowGMs = lowGEvidence ? sampleDeltaMs : 0;
              spinMs = spinEvidence ? sampleDeltaMs : 0;
              strongSpinMs = strongSpinEvidence ? sampleDeltaMs : 0;
              minTotalG = totalG;
              peakGyroDps = gyroDps;
              peakImpactG = totalG;

            }
            break;

          case THROW_STATE_AIRBORNE_CANDIDATE: {
            bool cleanLowGValid = lowGMs >= THROW_MIN_LOW_G_MS && minTotalG <= THROW_CLEAN_LOW_G_THRESHOLD;
            bool sustainedLowGValid = lowGMs >= THROW_MIN_FLIGHT_MS;
            bool lowGValid = cleanLowGValid || sustainedLowGValid;
            bool normalSpinDurationValid = spinMs >= THROW_MIN_SPIN_MS;
            bool normalSpinReleaseEvidence = minTotalG <= THROW_SPIN_RELEASE_G || lowGMs >= THROW_SPIN_MIN_LOW_G_MS;
            bool normalSpinValid = normalSpinDurationValid && normalSpinReleaseEvidence;
            bool strongSpinDurationValid = strongSpinMs >= THROW_MIN_STRONG_SPIN_MS;
            bool strongSpinReleaseEvidence = minTotalG <= THROW_STRONG_SPIN_RELEASE_G;
            bool strongSpinValid = strongSpinDurationValid && strongSpinReleaseEvidence;

            if (lowGValid || normalSpinValid || strongSpinValid) {
              state = THROW_STATE_WAIT_IMPACT;
              impactWindowStartedAt = now;

            } else if (!airborneEvidence && totalG > THROW_EXIT_LOW_G) {
              state = THROW_STATE_IDLE;
              stateStartedAt = 0;
              impactWindowStartedAt = 0;
              lowGMs = 0;
              spinMs = 0;
              strongSpinMs = 0;
              minTotalG = 99.0f;
              peakGyroDps = 0.0f;
              peakImpactG = 0.0f;
            } else if (now - stateStartedAt > THROW_IMPACT_WINDOW_MS) {
              state = THROW_STATE_IDLE;
              stateStartedAt = 0;
              impactWindowStartedAt = 0;
              lowGMs = 0;
              spinMs = 0;
              strongSpinMs = 0;
              minTotalG = 99.0f;
              peakGyroDps = 0.0f;
              peakImpactG = 0.0f;
            }
            break;
          }

          case THROW_STATE_WAIT_IMPACT:
            if (totalG >= THROW_IMPACT_G) {
              unsigned long flightMs = now - stateStartedAt;
              unsigned long impactDelayMs = now - impactWindowStartedAt;
              float impactRiseG = totalG - previousTotalG;
              bool impactSharp = impactRiseG >= THROW_IMPACT_RISE_G;
              bool hardImpact = totalG >= THROW_HARD_IMPACT_G;

              if (flightMs < THROW_MIN_FLIGHT_MS || impactDelayMs < THROW_MIN_IMPACT_DELAY_MS) {
                state = THROW_STATE_IDLE;
                stateStartedAt = 0;
                impactWindowStartedAt = 0;
                lowGMs = 0;
                spinMs = 0;
                strongSpinMs = 0;
                minTotalG = 99.0f;
                peakGyroDps = 0.0f;
                peakImpactG = 0.0f;
              } else if (!impactSharp && !hardImpact) {
                state = THROW_STATE_IDLE;
                stateStartedAt = 0;
                impactWindowStartedAt = 0;
                lowGMs = 0;
                spinMs = 0;
                strongSpinMs = 0;
                minTotalG = 99.0f;
                peakGyroDps = 0.0f;
                peakImpactG = 0.0f;
              } else {
                throwDetected = true;
                taskShouldRun = false;

              }
            } else if (now - impactWindowStartedAt > THROW_IMPACT_WINDOW_MS) {
              state = THROW_STATE_IDLE;
              stateStartedAt = 0;
              impactWindowStartedAt = 0;
              lowGMs = 0;
              spinMs = 0;
              strongSpinMs = 0;
              minTotalG = 99.0f;
              peakGyroDps = 0.0f;
              peakImpactG = 0.0f;
            }
            break;
        }

        previousTotalG = totalG;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(THROW_SAMPLE_INTERVAL_MS));
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
