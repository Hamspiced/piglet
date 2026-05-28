#pragma once
#include <Arduino.h>
#include <esp_wifi_types.h>

void doScanOnce();
String authModeToString(wifi_auth_mode_t m);
