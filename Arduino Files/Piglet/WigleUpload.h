#pragma once
#include <Arduino.h>

bool     wigleTestToken();
bool     uploadFileToWigle(const String& path);
uint32_t uploadAllCsvsToWigle(int maxFiles = -1);  // -1 = no limit
void     wigleLoadHistory();
