#pragma once
#include <Arduino.h>

void startAP();
bool connectSTA(uint32_t timeoutMs);
void stopAPIfAllowed();
void handleStaTransitions();
bool shouldPauseScanning();
