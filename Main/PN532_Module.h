#ifndef PN532_MODULE_H
#define PN532_MODULE_H

#include <Arduino.h>
#include <Adafruit_PN532.h>
#include "Config.h"

void initPN532();
void powerDownPN532();
String scanUID(); // Fungsi untuk mendapatkan ID kartu sebagai String
String scanUIDPeriodik(uint32_t intervalMs = 100);

#endif