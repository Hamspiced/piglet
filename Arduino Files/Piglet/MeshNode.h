#pragma once
#include <Arduino.h>
#include "OtaProto.h"

// ================================================================
//  MeshNode — JCMK-compatible ESP-Now wardriving node (page 5)
//  Compatible with justcallmekoko/ESP32DualBandWardriver core role.
// ================================================================

// JCMK channel table (shared so Display can show range labels)
extern const uint8_t JCMK_ESPNOW_CH;
extern const uint8_t JCMK_NUM_CHANNELS;
extern const uint8_t JCMK_CHANNELS[];

// Node state (read from Display.cpp to render page 5)
extern bool     meshNodeActive;
extern bool     jcmkHaveCore;
extern uint8_t  jcmkCoreMac[6];
extern uint8_t  jcmkAssignVer;
extern uint8_t  jcmkStartIdx;
extern uint8_t  jcmkEndIdx;
extern uint32_t jcmkNetworksFound;
extern uint32_t jcmkSentCount;
// Count of ESP-Now sends that the radio/driver itself reported as failed
// (via the send-status callback) -- distinguishes "attempted" from
// "actually transmitted" since broadcast frames have no delivery ACK at all.
extern uint32_t jcmkSendFailCount;

// Lifecycle — called from Piglet.ino on page enter/exit
void enterNodeMode();
void exitNodeMode();

// Loop tick — call every loop() iteration while on page 5
void nodeModeTick();

// OLED page renderer — called from Display.cpp updateOLED() (handles both modes)
void drawPageMeshNode();

// ================================================================
//  MeshCore — Core (coordinator) role
//  Long-press on page 5 while in Node mode activates Core mode;
//  long-press again returns to Node mode.
// ================================================================

#define CORE_MAX_NODES 12

struct CoreNodeInfo {
  bool     active;
  uint8_t  mac[6];
  uint8_t  startIdx;   // index into JCMK_CHANNELS[]
  uint8_t  endIdx;
  uint32_t lastHbMs;
  uint32_t recordsRx;
  bool     isBiscuit;  // true = Biscuit Node protocol (requires full-size 212-byte packets)
  bool     isPiglet;   // true = confirmed genuine Piglet node (via marker handshake) --
                        // only these participate in deterministic transmit-slot scheduling
  uint8_t  slotIndex;  // this node's transmit slot among other Piglet nodes (valid when isPiglet)
  uint8_t  boardType;  // PigletBoardType (PIGLET_BOARD_UNKNOWN unless isPiglet) -- which
                        // /Firmware/<Board>.ota.bin image this node can be pushed
  char     version[9]; // node's reported FIRMWARE_VERSION, empty if unknown (pre-version node)
  bool     updateDeclined;     // user let the on-device update prompt time out for this node
  bool     autoUpdateAttempted; // headless-node auto-push already tried this session (no retry loop)
};

// Core state (read from Display.cpp for page 5 rendering)
extern bool          meshCoreActive;
extern uint32_t      coreRecordsRx;
extern uint8_t       coreNodeCount;
extern CoreNodeInfo  coreNodes[CORE_MAX_NODES];

void enterCoreMode();
void exitCoreMode();
void coreModeTick();

// ================================================================
//  Manual per-node channel-range override (Web UI)
//  Lets an operator pin a specific node (matched by MAC) to a fixed channel
//  range instead of the automatic even split in coreReassignChannels() --
//  e.g. a node with a directional antenna aimed at a specific band. Persisted
//  to /mesh_channel_overrides.cfg so it survives a reboot; loaded once in
//  enterCoreMode(). chStart/chEnd are actual channel numbers (e.g. 1, 149),
//  not JCMK_CHANNELS[] indices -- both must exactly match an entry in
//  JCMK_CHANNELS[] or the call fails.
// ================================================================
uint8_t meshChOverrideGetCount();
bool    meshChOverrideGetAt(uint8_t idx, uint8_t* macOut, uint16_t& chStartOut, uint16_t& chEndOut);
bool    meshChOverrideSet(const uint8_t* mac, int chStart, int chEnd);
bool    meshChOverrideClear(const uint8_t* mac);

// ================================================================
//  Core-pushed OTA update of a connected mesh node (PIG_MSG_OTA_*)
//  Ported from olosnoks/warhog's coreOta* (Core role) -- streams a
//  node's plain app image over the existing ESP-NOW link, no USB. The
//  node verifies a SHA-256 of the whole image before switching boot
//  partition, so a bad transfer leaves it running its old firmware.
//  Images are staged on the Core's own SD card at /Firmware/<Board>.ota.bin
//  (one per PigletBoardType) and loaded into PSRAM once at boot
//  (coreOtaPreload) -- call it from setup() right after the SD card mounts.
//  coreOtaStart() just arms a transfer; coreModeTick() drives it.
// ================================================================
enum class CoreOtaPhase : uint8_t { Idle, Starting, Sending, Done, Failed };
struct CoreOtaStatus {
  CoreOtaPhase phase;
  int8_t       nodeIdx;      // roster index being updated, -1 if none
  uint32_t     sentBytes;    // bytes acknowledged by the node so far
  uint32_t     totalBytes;
  char         msg[28];      // short human-readable result / failure reason
  char         loadMsg[28];  // per-board-type summary of what's staged on SD (e.g. "XIAO:ok Node:-")
};
void          coreOtaPreload();                // call once at boot, right after the SD card mounts
bool          coreOtaStart(uint8_t nodeIdx);   // false if it could not start (see coreOtaStatus().msg)
void          coreOtaAbort();
CoreOtaStatus coreOtaStatus();

// ================================================================
//  On-device "Update Nodes through Core?" prompt (XIAO nodes only --
//  headless PigletNode nodes are instead auto-pushed silently when
//  cfg.autoUpdateHeadlessNodes is enabled). Read by drawPageMeshNode()
//  (Display.cpp) to render the countdown/progress, and by the mesh-page
//  button handler (Piglet.ino) to know whether a single press should be
//  consumed as "confirm" instead of advancing the page.
// ================================================================
struct CoreUpdatePromptStatus {
  bool     promptActive;       // true while the 30s confirm window is open
  uint32_t promptRemainingMs;  // time left in the confirm window, valid when promptActive
  uint8_t  pendingCount;       // nodes queued for update (including the one currently pushing)
  bool     pushActive;         // a coreOta transfer for this queue is in flight
  uint8_t  pushNodeIdx;        // coreNodes[] index being pushed, valid when pushActive
};
CoreUpdatePromptStatus coreUpdatePromptStatus();
void                   coreUpdatePromptConfirm();  // call when a single press lands while promptActive
