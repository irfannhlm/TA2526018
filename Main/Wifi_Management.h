// Wifi_Management.h

#ifndef WIFI_MANAGEMENT_H
#define WIFI_MANAGEMENT_H

#include <Arduino.h>
#include "Config.h"

// ── Fungsi ──
void initWifiPortal();
void bukaPortal();
void handleWifiPortal();
void stopWifiPortal();
bool isWifiPortalActive();
#endif
