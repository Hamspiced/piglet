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

// WDGoWars async job results are checked in the background instead of
// blocking the upload loop. Call wdgwarsServicePendingJobs() periodically
// from loop() to check on queued jobs (rate-limited internally — at most
// one short network round-trip per call). Call wdgwarsDrainPendingJobs()
// with a time budget right before intentionally dropping the STA
// connection (e.g. before autoStartAfterUpload or entering mesh Core mode)
// to give pending jobs a last chance to resolve.
void     wdgwarsServicePendingJobs();
void     wdgwarsDrainPendingJobs(uint32_t budgetMs);
