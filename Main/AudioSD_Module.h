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

enum QuestionStatus : uint8_t {
  QUESTION_OK = 0,
  QUESTION_NONE,
  QUESTION_SD_ERROR
};

enum DataLoadStatus : uint8_t {
  DATA_LOAD_OK = 0,
  DATA_LOAD_EMPTY,
  DATA_LOAD_SD_ERROR
};

enum RecordingResult : uint8_t {
  RECORDING_OK = 0,
  RECORDING_THROWN,
  RECORDING_SD_ERROR
};

void initAudioSD();
void deinitAudio();
bool ensureSdReady(const char* context, bool forceProbe = false);
bool isSdReady();
void pauseSdRecovery();
void resumeSdRecovery();
void requestSdRecovery(const char* context);
void markSdLost(const char* reason);
void clearPreRecordBuffer();
void drainAudioInput();
QuestionStatus cekStatusPertanyaan();
RecordingResult rekamSuara(String uid, unsigned long waktuBerpikir);
bool tulisMetadata(String filename, String uid, unsigned long waktuBerpikir, int currentQ, int currentA);
void resetAudioFilters();
void resetVadState();
void muteVad(unsigned long ms);
bool updateAudioPreBufferAndVad(int samples, float &maxLoudness);
unsigned long getVadFirstSpeechMs();
unsigned long getVadFirstSoftSpeechMs();

#endif
