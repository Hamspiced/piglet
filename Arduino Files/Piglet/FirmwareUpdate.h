#pragma once
#include <Arduino.h>

// ================================================================
//  Self-update from GitHub Releases
//  Checks https://api.github.com/repos/Hamspiced/piglet/releases/latest
//  for a newer FIRMWARE_VERSION. The Web UI drives a two-step flow:
//  download this board's own release asset to /Firmware/ on the SD card
//  (non-blocking, ticked from loop() so progress can be polled live), then
//  a separate, fast local flash step reads it back off the SD card and
//  installs it via Update.h. Self-update only ever touches this board's
//  own asset -- staging the OTHER two boards' assets for the mesh
//  Core-push engine is a separate, explicit action (fwStageNodeFirmware(),
//  "Store Piglet Node Firmware").
// ================================================================

// Boot-time check. Call once from setup(), right after staOk is known and
// BEFORE the boot-upload block -- boot-only, never tied to the mid-drive
// "home network seen" auto-upload path (that's checkHomeNetworkReturn() in
// WiFiManager.cpp, a separate code path). Silent and non-fatal on any
// failure (offline, STA down, parse error, no matching asset): boot
// continues normally either way.
void fwCheckForUpdate();

// Web UI-triggered (POST /update/downloadStart): begins downloading this
// board's own release asset to /Firmware/<OwnAsset>.ota.bin on the SD card.
// Returns immediately -- the transfer is driven by fwDownloadTick(), called
// every loop() iteration, so the Web UI can poll live progress via
// /update/status while it runs. A no-op if a download is already running.
void fwDownloadStart();

// Call once per loop() iteration. No-op unless a download is in progress.
// Does a bounded slice of work per call (the initial connect, or one
// read+write chunk of the body) so server.handleClient() keeps running
// between calls.
void fwDownloadTick();

// Web UI-triggered (POST /update/downloadCancel): aborts an in-progress
// download and deletes the partial file. No-op if nothing is downloading.
void fwDownloadCancel();

// Web UI-triggered (POST /update/clearFirmware): deletes every file
// currently staged in /Firmware/ on the SD card -- this board's own
// downloaded-but-not-yet-installed image, plus anything staged for the mesh
// Core-push engine. Refuses (returns false, nothing deleted) while a
// download or install is actively using that directory. Resets
// fwDownloaded/progress fields back to "nothing staged" on success.
bool fwClearFirmwareFolder();

// Full SD path of this board's own staged release asset (e.g.
// "/Firmware/Piglet.ota.bin"). Exposed so boot-time code can check whether
// a completed download is waiting to be installed without needing to know
// the exact filename (kept private to FirmwareUpdate.cpp otherwise).
String fwOwnAssetPath();

// Web UI-triggered (POST /update/installFromSD): reads the already-
// downloaded /Firmware/<OwnAsset>.ota.bin back off the SD card and flashes
// it via Update.h, restarting on success. Purely local (no network), so
// unlike the download step this stays a simple blocking call. Returns
// false (current firmware keeps running) on any failure before the final
// ESP.restart(). Fails if fwDownloaded is not already true.
bool fwInstallFromSD();

// Web UI-triggered "Store Piglet Node Firmware" (POST /update/stageNode):
// downloads just the PigletNode.ota.bin release asset to /Firmware/ on the
// SD card, independent of whether this board's own firmware is current --
// a Core already running the latest release still needs this to stage
// firmware for headless nodes. Runs fwCheckForUpdate() first if a check
// hasn't resolved asset URLs yet. Blocking (small, single-shot download).
// Caller (WebUI.cpp) should call coreOtaPreload() afterwards on success so
// the freshly staged image is loaded into PSRAM without requiring a reboot.
bool fwStageNodeFirmware();

// True once fwCheckForUpdate() has resolved a download URL for the
// PigletNode release asset -- lets the Web UI know "Store Piglet Node
// Firmware" has something to fetch before the button is clicked.
bool fwHasNodeAsset();

// ---- Status, read by WebUI.cpp for the Firmware Update card ----
extern bool     fwUpdateChecked;         // a boot-time check has completed
extern bool     fwUpdateAvailable;       // newer version found on GitHub
extern String   fwAvailableVersion;      // e.g. "v3.1" -- the latest release tag, whether or not it's newer
extern String   fwReleaseNotes;          // short excerpt of the release body
extern String   fwCheckError;            // last Check-for-Update failure reason, if any -- fwUpdateAvailable/
                                          // fwAvailableVersion are left at their last known-good values on
                                          // failure, never blanked out, so a transient failed recheck can't
                                          // make a previously-detected update disappear from the Web UI

extern bool     fwDownloading;           // true while fwDownloadTick() is actively transferring
extern bool     fwDownloaded;            // true once a file is fully downloaded and ready to install
extern String   fwDownloadStage;         // human-readable download progress stage
extern uint32_t fwDownloadedBytes;
extern uint32_t fwDownloadTotalBytes;
extern String   fwDownloadError;         // last download failure reason, if any

extern bool     fwInstalling;            // true while fwInstallFromSD() runs
extern String   fwInstallError;          // last flash-install failure reason, if any

extern String   fwLog;                   // capped verbose log, newest at the end

extern bool     fwStagingNode;           // true while fwStageNodeFirmware() runs
extern String   fwStageNodeError;        // last "Store Piglet Node Firmware" failure reason, if any
