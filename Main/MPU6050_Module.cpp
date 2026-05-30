#include "MPU6050_Module.h"

Adafruit_MPU6050 mpu;
volatile bool motionDetected = false;
volatile bool throwDetected  = false;  // Dibaca oleh loop rekam

static TaskHandle_t throwTaskHandle = nullptr;
static volatile bool taskShouldRun  = false;

void IRAM_ATTR mpuISR() {
    motionDetected = true;
}

void initMPU6050() {
  if (!mpu.begin()) {
    Serial.println("MPU6050 Gagal");
    return;
  }
  
  // Range 8G agar lebih toleran terhadap guncangan saat ditangkap
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);
  
  Serial.println("MPU6050 Ready");

  mpu.setMotionDetectionThreshold(25); // Batas guncangan pemicu interrupt
  mpu.setMotionDetectionDuration(5);

  // MPU kirim sinyal interrupt saat ada gerakan apapun
  mpu.setHighPassFilter(MPU6050_HIGHPASS_0_63_HZ);
  mpu.setMotionInterrupt(true);

  pinMode(MPU_INT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(MPU_INT_PIN), mpuISR, RISING);
}

static void throwDetectionTask(void* param) {
    static unsigned long flyStartTime = 0;
    flyStartTime = 0; // ← tambah reset eksplisit tiap task start

    while (taskShouldRun) {
        if (motionDetected) {
            sensors_event_t a, g, temp;
            mpu.getEvent(&a, &g, &temp);

            float ax = a.acceleration.x / 9.81f;
            float ay = a.acceleration.y / 9.81f;
            float az = a.acceleration.z / 9.81f;
            float totalG = sqrtf(ax*ax + ay*ay + az*az);

            if (totalG < FLYING_THRESHOLD) {
                if (flyStartTime == 0) flyStartTime = millis();
                if (millis() - flyStartTime > FLYING_DURATION_MS) {
                    throwDetected = true;
                }
            } else {
                flyStartTime = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    throwTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

void startThrowDetectionTask() {
    throwDetected  = false;
    motionDetected = false;
    taskShouldRun  = true;
    // Pinned ke Core 0; loop rekam + I2S ada di Core 1 (Arduino default)
    xTaskCreatePinnedToCore(
        throwDetectionTask, "ThrowDetect",
        2048, nullptr, 1,
        &throwTaskHandle, 0
    );
}

void stopThrowDetectionTask() {
    taskShouldRun = false;
    // Task akan exit sendiri di iterasi berikutnya
}


void resetFlyingFlag() {
    throwDetected  = false;
    motionDetected = false;
}