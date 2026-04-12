#ifndef PN532_MODULE_H
#define PN532_MODULE_H

#include <Arduino.h>
#include <Adafruit_PN532.h>
#include "Config.h"

void initPN532();
String scanUID(); // Fungsi untuk mendapatkan ID kartu sebagai String

#endif