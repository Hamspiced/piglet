#pragma once
#include <Arduino.h>

void doScanOnce();

// GPS last-known-good position cache (maintained by loop(), read by Scanner)
extern bool     lastGpsValid;
extern double   lastLat, lastLon, lastAlt, lastAcc;
extern uint32_t lastGpsValidMs;
extern const uint32_t GPS_CACHE_MAX_MS;
