#ifndef AUDIOSD_MODULE_H
#define AUDIOSD_MODULE_H

#include <Arduino.h>
#include <LiquidCrystal_I2C.h>
#include <driver/i2s_std.h>
#include "Config.h"

// Agar modul bisa pakai LCD dari .ino
extern LiquidCrystal_I2C lcd; 

// Agar .ino bisa pakai handle I2S dan Buffer dari .cpp
extern i2s_chan_handle_t rx_handle; 
extern int16_t *preRecordBuffer;
extern int16_t *processed_buffer;
extern int32_t *raw_i2s_buffer;
extern int bufferHead;
extern bool bufferIsFull;

void initAudioSD();
float processAudioBuffer(int32_t *input, int16_t *output, int samples, long &sumLoudness);
String getFastFilename(String prefix);
void rekamSuara(String uid, unsigned long waktuBerpikir);
void tulisMetadata(String filename, String uid, unsigned long waktuBerpikir);
void resetAudioFilters();

#endif