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

// Wardrift upload (wardrift.net) — uses X-API-Key header, POST /v1/ingest/signals.
// The CSV file is uploaded as-is (raw body, Content-Type: text/csv, plus an
// X-File-Name header) -- no multipart wrapper, no per-row parsing. This
// endpoint caps files at 10 MiB (10,485,760 bytes); larger CSVs are skipped
// here (still attempted on WDGoWars/WiGLE). Runs 2nd in the upload chain:
// after WDGoWars, before WiGLE.
bool     wardriftTestKey();                           // posts a header-only test CSV — 200 or 400 = valid key
bool     uploadFileToWardrift(const String& path);
uint32_t uploadAllCsvsToWardrift(int maxFiles = -1);  // Wardrift-only batch upload
