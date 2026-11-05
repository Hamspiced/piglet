#pragma once
#include <Arduino.h>

// ================================================================
//  Self-update from GitHub Releases
//  Checks https://api.github.com/repos/Hamspiced/piglet/releases/latest
//  for a newer FIRMWARE_VERSION and, on a Web UI button click, downloads
//  and flashes this board's own release asset via Update.h. The other two
//  boards' assets are staged into /Firmware/ on the SD card in the same
//  pass, so the mesh Core-push OTA engine (MeshNode.cpp) can push them to
//  connected nodes without the operator ever touching an SD card.
// ================================================================

// Boot-time check. Call once from setup(), right after staOk is known and
// BEFORE the boot-upload block -- boot-only, never tied to the mid-drive
// "home network seen" auto-upload path (that's checkHomeNetworkReturn() in
// WiFiManager.cpp, a separate code path). Silent and non-fatal on any
// failure (offline, STA down, parse error, no matching asset): boot
// continues normally either way.
void fwCheckForUpdate();

// Web UI-triggered install (POST /update/install): downloads this board's
// own release asset and flashes it via Update.h, restarting on success.
// Also stages the other two boards' .ota.bin assets into /Firmware/ on the
// SD card (delete-before-write, so an interrupted write can't leave a
// corrupt hybrid file) for the mesh Core-push engine. Blocking, same
// pattern as the existing upload handlers in WebUI.cpp. Returns false
// (current firmware keeps running) on any failure before the final
// ESP.restart().
bool fwInstallUpdate();

// Web UI-triggered "Store Piglet Node Firmware" (POST /update/stageNode):
// downloads just the PigletNode.ota.bin release asset to /Firmware/ on the
// SD card, independent of whether this board's own firmware is current --
// a Core already running the latest release still needs this to stage
// firmware for headless nodes. Runs fwCheckForUpdate() first if a check
// hasn't resolved asset URLs yet. Blocking, same pattern as fwInstallUpdate().
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
extern bool     fwInstalling;            // true while fwInstallUpdate() runs
extern String   fwInstallStage;          // human-readable progress stage
extern uint32_t fwInstallDownloadedBytes;
extern uint32_t fwInstallTotalBytes;
extern String   fwInstallError;          // last failure reason, if any

extern bool     fwStagingNode;           // true while fwStageNodeFirmware() runs
extern String   fwStageNodeError;        // last "Store Piglet Node Firmware" failure reason, if any
