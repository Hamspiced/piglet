#pragma once
#include <Arduino.h>

// Tracks, per CSV file, which upload services it has already succeeded
// against -- so a failure against one service (e.g. WDGoWars) never blocks
// or re-triggers the others (WiGLE/Wardrift) for that same file, and the
// file keeps being retried only against whichever service(s) are still
// pending for it. Persisted in a small plain-text ledger at
// /logs/.upload_state.cfg (one line per filename: "name|wigle|wdgw|wardrift",
// each field 1=done or 0=pending), consistent with the rest of this
// project's plain key=value config style.

enum class UploadService { Wigle, Wdgw, Wardrift };

struct FileUploadState {
  bool wigleDone    = false;
  bool wdgwDone     = false;
  bool wardriftDone = false;
};

// Loads the recorded state for `filename` (basename, not full path). A
// service that isn't configured (no API key/token set) is always reported
// as done, so a file is never held back by a service the user never
// enabled -- callers don't need to special-case that themselves.
FileUploadState loadUploadState(const String& filename);

// Records that `svc` succeeded for `filename`, creating a ledger entry if
// one doesn't exist yet. Rewrites the whole ledger file (small/infrequent,
// same pattern as saveConfigToSD()).
void markServiceDone(const String& filename, UploadService svc);

// Deletes any ledger entry for `filename` -- call once a file has been
// moved to /uploaded/ (or deleted) so the ledger doesn't grow unbounded.
void clearUploadState(const String& filename);

// True once every *configured* service is done for this file (unconfigured
// services are already reported as done by loadUploadState()).
bool allConfiguredServicesDone(const FileUploadState& st);

// Drops ledger entries for filenames no longer present in /logs (already
// moved to /uploaded/ or deleted outside the normal flow). Call at boot
// alongside deleteEmptyCsvs().
void pruneUploadState();
