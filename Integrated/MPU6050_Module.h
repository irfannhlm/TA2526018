#ifndef MPU6050_MODULE_H
#define MPU6050_MODULE_H

#include <Arduino.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include "Config.h"

void initMPU6050();
bool isDeviceFlying(); // Fungsi deteksi lemparan
void resetFlyingFlag();

#endif