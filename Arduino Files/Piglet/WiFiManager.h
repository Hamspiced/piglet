#pragma once
#include <Arduino.h>

void startAP();
bool connectSTA(uint32_t timeoutMs);
void stopAPIfAllowed();
void handleStaTransitions();
bool shouldPauseScanning();

// Call once per loop() (outside mesh mode). If the home SSID was seen in the
// last scan (Scanner.cpp sets homeNetworkSeen), connects to it and runs the
// same upload flow used at boot. Normal wardriving resumes automatically via
// handleStaTransitions() once the home network connection is lost.
void checkHomeNetworkReturn();
