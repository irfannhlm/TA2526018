// Communication.h

#ifndef COMMUNICATION_H
#define COMMUNICATION_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "esp_wpa2.h"
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "Config.h"

// ── Fungsi WiFi ──
void startWiFi();
void stopWiFi();

// ── Fungsi MQTT ──
void handleMQTT();
void kirimStatus(String status_device);
void sendRegistration(String uid);

// ── Fungsi Sinkronisasi ──
void prosesSinkronisasiSD(const String& targetKelas);
void prosesSinkronisasiAudio(const String& targetKelas);

// ── MQTT Client (pakai WiFiClientSecure untuk HiveMQ TLS) ──
extern WiFiClientSecure espClient;
extern PubSubClient     mqttClient;

// ── State Sinkronisasi TXT ──
extern bool   sdSyncAktif;
extern String sdTargetKelas;

// ── State Sinkronisasi Audio WAV ──
extern bool   audioSyncAktif;
extern String audioTargetKelas;

#endif
