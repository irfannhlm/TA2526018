#ifndef MPU6050_MODULE_H
#define MPU6050_MODULE_H

#include <Arduino.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include "Config.h"
//#include "I2C_Handler.h"

void initMPU6050();
void resetFlyingFlag();

void startThrowDetectionTask();     // Untuk dipanggil saat mulai rekam
void stopThrowDetectionTask();      // Untuk dipanggil setelah rekam

extern volatile bool throwDetected; // Flag hasil task

#endif
