#include "UploadState.h"
#include "Globals.h"
#include <SD.h>
#include <vector>

static const char* STATE_PATH = "/logs/.upload_state.cfg";

namespace {

struct LedgerEntry {
  String filename;
  bool   wigle;
  bool   wdgw;
  bool   wardrift;
};

bool readLedger(std::vector<LedgerEntry>& out) {
  if (!sdOk || !SD.exists(STATE_PATH)) return false;
  File f = SD.open(STATE_PATH, FILE_READ);
  if (!f) return false;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int p1 = line.indexOf('|');
    if (p1 < 0) continue;
    int p2 = line.indexOf('|', p1 + 1);
    if (p2 < 0) continue;
    int p3 = line.indexOf('|', p2 + 1);
    if (p3 < 0) continue;

    LedgerEntry e;
    e.filename = line.substring(0, p1);
    e.wigle    = line.substring(p1 + 1, p2) == "1";
    e.wdgw     = line.substring(p2 + 1, p3) == "1";
    e.wardrift = line.substring(p3 + 1) == "1";
    out.push_back(e);
  }
  f.close();
  return true;
}

// Rewrites the whole ledger from `entries`. An empty list removes the file
// entirely rather than leaving a zero-byte ledger behind.
bool writeLedger(const std::vector<LedgerEntry>& entries) {
  if (!sdOk) return false;
  if (SD.exists(STATE_PATH)) SD.remove(STATE_PATH);
  if (entries.empty()) return true;

  File f = SD.open(STATE_PATH, FILE_WRITE);
  if (!f) {
    Serial.println("[UPLOADSTATE] Failed to open ledger for write");
    return false;
  }
  for (const auto& e : entries) {
    f.print(e.filename); f.print('|');
    f.print(e.wigle ? '1' : '0'); f.print('|');
    f.print(e.wdgw ? '1' : '0'); f.print('|');
    f.println(e.wardrift ? '1' : '0');
  }
  f.flush();
  f.close();
  return true;
}

}  // namespace

FileUploadState loadUploadState(const String& filename) {
  FileUploadState st;
  // A service with no key/token configured is always "done" so a file is
  // never held back waiting on a service the user never enabled.
  st.wigleDone    = cfg.wigleBasicToken.length() < 8;
  st.wdgwDone     = cfg.wdgwarsApiKey.length()   < 8;
  st.wardriftDone = cfg.wardriftApiKey.length()  < 8;

  std::vector<LedgerEntry> entries;
  if (!readLedger(entries)) return st;

  for (const auto& e : entries) {
    if (e.filename != filename) continue;
    if (e.wigle)    st.wigleDone    = true;
    if (e.wdgw)     st.wdgwDone     = true;
    if (e.wardrift) st.wardriftDone = true;
    break;
  }
  return st;
}

void markServiceDone(const String& filename, UploadService svc) {
  std::vector<LedgerEntry> entries;
  readLedger(entries);

  bool found = false;
  for (auto& e : entries) {
    if (e.filename != filename) continue;
    found = true;
    if (svc == UploadService::Wigle)    e.wigle    = true;
    if (svc == UploadService::Wdgw)     e.wdgw     = true;
    if (svc == UploadService::Wardrift) e.wardrift = true;
    break;
  }
  if (!found) {
    LedgerEntry e;
    e.filename = filename;
    e.wigle    = (svc == UploadService::Wigle);
    e.wdgw     = (svc == UploadService::Wdgw);
    e.wardrift = (svc == UploadService::Wardrift);
    entries.push_back(e);
  }
  writeLedger(entries);
}

void clearUploadState(const String& filename) {
  std::vector<LedgerEntry> entries;
  if (!readLedger(entries)) return;

  for (size_t i = 0; i < entries.size(); i++) {
    if (entries[i].filename == filename) {
      entries.erase(entries.begin() + i);
      writeLedger(entries);
      return;
    }
  }
}

bool allConfiguredServicesDone(const FileUploadState& st) {
  return st.wigleDone && st.wdgwDone && st.wardriftDone;
}

void pruneUploadState() {
  std::vector<LedgerEntry> entries;
  if (!readLedger(entries)) return;

  std::vector<LedgerEntry> kept;
  kept.reserve(entries.size());
  for (const auto& e : entries) {
    if (SD.exists(String("/logs/") + e.filename)) kept.push_back(e);
  }
  if (kept.size() != entries.size()) writeLedger(kept);
}
