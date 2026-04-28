#pragma once
#include <Arduino.h>

bool     wigleTestToken();
bool     uploadFileToWigle(const String& path);
uint32_t uploadAllCsvsToWigle(int maxFiles = -1);  // -1 = no limit
void     deleteEmptyCsvs();  // scan /logs and delete header-only files

// WDGoWars upload (wdgwars.pl) — uses X-API-Key header, /api/upload-csv
bool     wdgwarsTestKey();                          // GET /api/me — 200 = valid
bool     uploadFileToWdgwars(const String& path);
uint32_t uploadAllCsvsToWdgwars(int maxFiles = -1); // WDGoWars-only batch upload
