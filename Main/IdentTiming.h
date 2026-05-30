// IdentTiming.h — Final, Serial Monitor only
// Pengukur kecepatan identifikasi RFID
// T1 = scanUID() return non-empty
// T2 = tepat sebelum buzzer/state change

#pragma once
#include <Arduino.h>
#include "esp_timer.h"

// ─────────────────────────────────────────────
//  KONFIGURASI
// ─────────────────────────────────────────────
#define IDENT_MAX_SAMPLES  20
#define IDENT_TOLERANCE_MS 1300
#define IDENT_MIN_PASS     5

// ─────────────────────────────────────────────
//  TIPE KEPUTUSAN
// ─────────────────────────────────────────────
enum IdentDecision {
  DEC_AUTHORIZED,
  DEC_INVALID,
  DEC_UNREGISTERED,
  DEC_BANNED,
  DEC_NO_QUESTION,
  DEC_SD_ERROR
};

static const char* decisionLabel[] = {
  "AUTHORIZED",
  "INVALID",
  "UNREGISTERED",
  "BANNED",
  "NO_QUESTION",
  "SD_ERROR"
};

// ─────────────────────────────────────────────
//  STATE INTERNAL
// ─────────────────────────────────────────────
static int64_t _t1_us      = 0;
static bool    _timerActive = false;
static int32_t _samples[IDENT_MAX_SAMPLES];
static int     _sampleCount = 0;
static int     _passCount   = 0;

// ─────────────────────────────────────────────
//  API
// ─────────────────────────────────────────────

inline void identTimer_start() {
  _t1_us       = esp_timer_get_time();
  _timerActive = true;
}

inline void identTimer_stop(IdentDecision decision) {
  if (!_timerActive) return;

  int64_t delta_us = esp_timer_get_time() - _t1_us;
  int32_t delta_ms = (int32_t)(delta_us / 1000);
  _timerActive     = false;
  bool pass        = (delta_ms <= IDENT_TOLERANCE_MS);

  if (_sampleCount < IDENT_MAX_SAMPLES)
    _samples[_sampleCount++] = delta_ms;
  if (pass) _passCount++;

  // ── Per-percobaan ──
  Serial.println(F("----------------------------------------"));
  Serial.printf("[#%02d] %-14s | %4d ms | %s\n",
                _sampleCount,
                decisionLabel[decision],
                delta_ms,
                pass ? "LULUS" : "GAGAL");

  // ── Auto-ringkasan tiap kelipatan 5 ──
  if (_sampleCount % IDENT_MIN_PASS == 0) {
    int32_t minMs = _samples[0], maxMs = _samples[0];
    int64_t sumMs = 0;
    for (int i = 0; i < _sampleCount; i++) {
      if (_samples[i] < minMs) minMs = _samples[i];
      if (_samples[i] > maxMs) maxMs = _samples[i];
      sumMs += _samples[i];
    }
    float avgMs = (float)sumMs / _sampleCount;
    bool  lulus = (_passCount == _sampleCount);

    Serial.println(F("========================================"));
    Serial.printf("  RINGKASAN %d PERCOBAAN\n", _sampleCount);
    Serial.println(F("----------------------------------------"));
    Serial.printf("  Lulus : %d / %d\n", _passCount, _sampleCount);
    Serial.printf("  Min   : %4d ms\n", minMs);
    Serial.printf("  Max   : %4d ms\n", maxMs);
    Serial.printf("  Avg   : %7.1f ms\n", avgMs);
    Serial.println(F("----------------------------------------"));
    Serial.printf("  VERDICT: %s\n", lulus ? "SISTEM LULUS" : "SISTEM GAGAL");
    Serial.println(F("========================================"));
  }
}

inline void identTimer_reset() {
  _sampleCount = 0;
  _passCount   = 0;
  _timerActive = false;
  Serial.println("[IDENT] Reset. Sesi baru dimulai.");
}