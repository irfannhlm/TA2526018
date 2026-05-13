#include "MPU6050_Module.h"

Adafruit_MPU6050 mpu;

volatile bool motionDetected = false; // Flag dari interrupt

// Fungsi ISR (Interrupt Service Routine)
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

  mpu.setMotionDetectionThreshold(35); // Batas guncangan pemicu interrupt
  mpu.setMotionDetectionDuration(5);

  // MPU kirim sinyal interrupt saat ada gerakan apapun
  mpu.setHighPassFilter(MPU6050_HIGHPASS_0_63_HZ);
  mpu.setMotionInterrupt(true);

  pinMode(MPU_INT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(MPU_INT_PIN), mpuISR, RISING);
}

bool isDeviceFlying() {
  // Jika tidak ada gerakan dari interrupt, langsung keluar
  if (!motionDetected) return false;
  
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);

  // Normalisasi ke unit 'g' (1g = 9.81 m/s^2)
  float ax = a.acceleration.x / 9.81;
  float ay = a.acceleration.y / 9.81;
  float az = a.acceleration.z / 9.81;

  // Magnitude Total G
  float totalG = sqrt(ax*ax + ay*ay + az*az);

  static unsigned long flyStartTime = 0;

  if (totalG < FLYING_THRESHOLD) {
    if (flyStartTime == 0) flyStartTime = millis();
    if (millis() - flyStartTime > FLYING_DURATION_MS) {
      return true; // Terdeteksi melayang cukup lama
    }
  } else {
    flyStartTime = 0;
  }

  return false;
}

void resetFlyingFlag() {
  motionDetected = false;
}