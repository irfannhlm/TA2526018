#include "MPU6050_Module.h"
#include "I2C_Handler.h"
#include <math.h>

Adafruit_MPU6050 mpu;

volatile bool throwDetected = false;  // Dibaca oleh loop rekam

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

#if THROW_DEBUG
static const char* throwStateName(ThrowDetectionState state) {
  switch (state) {
    case THROW_STATE_IDLE:
      return "IDLE";
    case THROW_STATE_AIRBORNE_CANDIDATE:
      return "AIRBORNE";
    case THROW_STATE_WAIT_IMPACT:
      return "WAIT_IMPACT";
  }

  return "UNKNOWN";
}

static void logThrowEvent(const char* event,
                          ThrowDetectionState state,
                          const char* reason,
                          float totalG,
                          float gyroDps,
                          unsigned long lowGMs,
                          unsigned long spinMs,
                          float minTotalG,
                          float peakGyroDps,
                          float peakImpactG) {
  Serial.printf(
    "[MPU] %s state=%s reason=%s totalG=%.2f gyroDps=%.0f lowMs=%lu spinMs=%lu minG=%.2f peakGyro=%.0f peakImpact=%.2f\n",
    event,
    throwStateName(state),
    reason,
    totalG,
    gyroDps,
    lowGMs,
    spinMs,
    minTotalG,
    peakGyroDps,
    peakImpactG
  );
}
#endif

void initMPU6050() {
  if (!mpu.begin()) {
    Serial.println("MPU6050 Gagal");
    return;
  }

  // Range 8G agar lebih toleran terhadap guncangan saat dilempar/ditangkap
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setGyroRange(MPU6050_RANGE_2000_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_44_HZ);

  Serial.println("MPU6050 Ready");
}

static void throwDetectionTask(void* param) {
  ThrowDetectionState state = THROW_STATE_IDLE;
  unsigned long stateStartedAt = 0;
  unsigned long impactWindowStartedAt = 0;
  unsigned long lastValidSampleAt = 0;
  unsigned long lastDebugAt = 0;
  unsigned long lowGMs = 0;
  unsigned long spinMs = 0;
  float minTotalG = 99.0f;
  float peakGyroDps = 0.0f;
  float peakImpactG = 0.0f;
  bool spinRejectLogged = false;

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
        bool airborneEvidence = lowGEvidence || spinEvidence;

        if (state != THROW_STATE_IDLE) {
          if (totalG < minTotalG) minTotalG = totalG;
          if (gyroDps > peakGyroDps) peakGyroDps = gyroDps;
          if (totalG > peakImpactG) peakImpactG = totalG;

          if (lowGEvidence) lowGMs += sampleDeltaMs;
          if (spinEvidence) spinMs += sampleDeltaMs;
        }

        switch (state) {
          case THROW_STATE_IDLE:
            if (airborneEvidence) {
              state = THROW_STATE_AIRBORNE_CANDIDATE;
              stateStartedAt = now;
              lowGMs = lowGEvidence ? sampleDeltaMs : 0;
              spinMs = spinEvidence ? sampleDeltaMs : 0;
              minTotalG = totalG;
              peakGyroDps = gyroDps;
              peakImpactG = totalG;
              spinRejectLogged = false;

#if THROW_DEBUG
              logThrowEvent("ENTER", state, lowGEvidence ? "low_g" : "gyro_spin", totalG, gyroDps, lowGMs, spinMs, minTotalG, peakGyroDps, peakImpactG);
#endif
            }
            break;

          case THROW_STATE_AIRBORNE_CANDIDATE: {
            bool cleanLowGValid = lowGMs >= THROW_MIN_LOW_G_MS && minTotalG <= THROW_CLEAN_LOW_G_THRESHOLD;
            bool sustainedLowGValid = lowGMs >= THROW_MIN_FLIGHT_MS;
            bool lowGValid = cleanLowGValid || sustainedLowGValid;
            bool spinDurationValid = spinMs >= THROW_MIN_SPIN_MS;
            bool spinReleaseEvidence = minTotalG <= THROW_SPIN_RELEASE_G || lowGMs >= THROW_SPIN_MIN_LOW_G_MS;
            bool spinValid = spinDurationValid && spinReleaseEvidence;

#if THROW_DEBUG
            if (spinDurationValid && !spinReleaseEvidence && !spinRejectLogged) {
              logThrowEvent("REJECT", state, "spin_rejected_no_release", totalG, gyroDps, lowGMs, spinMs, minTotalG, peakGyroDps, peakImpactG);
              spinRejectLogged = true;
            }
#endif

            if (lowGValid || spinValid) {
              state = THROW_STATE_WAIT_IMPACT;
              impactWindowStartedAt = now;

#if THROW_DEBUG
              const char* validReason = "spin_valid_release";
              if (cleanLowGValid) {
                validReason = "low_g_valid";
              } else if (sustainedLowGValid) {
                validReason = "low_g_sustained";
              }
              logThrowEvent("ENTER", state, validReason, totalG, gyroDps, lowGMs, spinMs, minTotalG, peakGyroDps, peakImpactG);
#endif
            } else if (!airborneEvidence && totalG > THROW_EXIT_LOW_G) {
#if THROW_DEBUG
              logThrowEvent("RESET", state, "candidate_lost", totalG, gyroDps, lowGMs, spinMs, minTotalG, peakGyroDps, peakImpactG);
#endif
              state = THROW_STATE_IDLE;
              stateStartedAt = 0;
              impactWindowStartedAt = 0;
              lowGMs = 0;
              spinMs = 0;
              minTotalG = 99.0f;
              peakGyroDps = 0.0f;
              peakImpactG = 0.0f;
              spinRejectLogged = false;
            } else if (now - stateStartedAt > THROW_IMPACT_WINDOW_MS) {
#if THROW_DEBUG
              logThrowEvent("RESET", state, "candidate_timeout", totalG, gyroDps, lowGMs, spinMs, minTotalG, peakGyroDps, peakImpactG);
#endif
              state = THROW_STATE_IDLE;
              stateStartedAt = 0;
              impactWindowStartedAt = 0;
              lowGMs = 0;
              spinMs = 0;
              minTotalG = 99.0f;
              peakGyroDps = 0.0f;
              peakImpactG = 0.0f;
              spinRejectLogged = false;
            }
            break;
          }

          case THROW_STATE_WAIT_IMPACT:
            if (totalG >= THROW_IMPACT_G) {
              unsigned long flightMs = now - stateStartedAt;
              unsigned long impactDelayMs = now - impactWindowStartedAt;

              if (flightMs < THROW_MIN_FLIGHT_MS || impactDelayMs < THROW_MIN_IMPACT_DELAY_MS) {
#if THROW_DEBUG
                logThrowEvent("RESET", state, "impact_too_soon", totalG, gyroDps, lowGMs, spinMs, minTotalG, peakGyroDps, peakImpactG);
#endif
                state = THROW_STATE_IDLE;
                stateStartedAt = 0;
                impactWindowStartedAt = 0;
                lowGMs = 0;
                spinMs = 0;
                minTotalG = 99.0f;
                peakGyroDps = 0.0f;
                peakImpactG = 0.0f;
                spinRejectLogged = false;
              } else {
                throwDetected = true;
                taskShouldRun = false;

#if THROW_DEBUG
                logThrowEvent("TRIGGER", state, "impact", totalG, gyroDps, lowGMs, spinMs, minTotalG, peakGyroDps, peakImpactG);
#endif
              }
            } else if (now - impactWindowStartedAt > THROW_IMPACT_WINDOW_MS) {
#if THROW_DEBUG
              logThrowEvent("RESET", state, "impact_timeout", totalG, gyroDps, lowGMs, spinMs, minTotalG, peakGyroDps, peakImpactG);
#endif
              state = THROW_STATE_IDLE;
              stateStartedAt = 0;
              impactWindowStartedAt = 0;
              lowGMs = 0;
              spinMs = 0;
              minTotalG = 99.0f;
              peakGyroDps = 0.0f;
              peakImpactG = 0.0f;
              spinRejectLogged = false;
            }
            break;
        }

#if THROW_DEBUG
        if (now - lastDebugAt >= 100UL) {
          lastDebugAt = now;
          logThrowEvent("SAMPLE", state, "periodic", totalG, gyroDps, lowGMs, spinMs, minTotalG, peakGyroDps, peakImpactG);
        }
#endif
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
