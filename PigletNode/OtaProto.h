#pragma once
#include <Arduino.h>

// ================================================================
//  OtaProto -- ESP-NOW wire format for Core-pushed mesh node
//  firmware updates. Adapted from olosnoks/warhog's
//  scheduler/pig_proto.h (OTA-only subset: this file omits that
//  project's observation-batch/yield-heartbeat/admin-ext structs,
//  which Piglet doesn't use).
//
//  Message types sit above JCMK's legacy 1..~10 range so real JCMK
//  hardware and Biscuit nodes/cores -- which only recognize their
//  own small set of type values -- silently ignore them.
// ================================================================

enum PigOtaMsgType : uint8_t {
  PIG_MSG_OTA_BEGIN = 0x25,  // Core -> node: start update (size, chunks, SHA-256)
  PIG_MSG_OTA_DATA  = 0x26,  // Core -> node: one in-order firmware chunk
  PIG_MSG_OTA_ACK   = 0x27   // node -> Core: cumulative ack / status
};

// Payload bytes per OTA_DATA frame -- keeps the whole frame under the
// ~250-byte ESP-NOW limit (magic+type+session+seq+len header is 9 bytes).
#define PIG_OTA_CHUNK  192
// Chunks the Core keeps in flight at once (go-back-N window).
#define PIG_OTA_WINDOW 4

enum PigOtaStatus : uint8_t {
  PIG_OTA_ST_READY     = 0,  // OTA_BEGIN accepted; send chunk 0
  PIG_OTA_ST_OK        = 1,  // progress: `next` = chunks written so far
  PIG_OTA_ST_DONE      = 2,  // image verified + boot partition switched; node is rebooting
  PIG_OTA_ST_ERR_SPACE = 3,  // image does not fit / no OTA partition (old single-slot layout)
  PIG_OTA_ST_ERR_WRITE = 4,  // flash write failed
  PIG_OTA_ST_ERR_SHA   = 5,  // SHA-256 mismatch (update discarded, node keeps running old firmware)
  PIG_OTA_ST_ERR_ARGS  = 6   // bad size / chunk count
};

typedef struct __attribute__((packed)) {
  char     magic[4];        // mesh magic -- "ENOW"
  uint8_t  type;            // PIG_MSG_OTA_BEGIN (0x25)
  uint8_t  session;         // update session id (changes every update attempt)
  uint32_t size;            // image size in bytes
  uint16_t chunks;          // ceil(size / PIG_OTA_CHUNK)
  uint8_t  sha256[32];      // SHA-256 of the whole image
} pig_ota_begin_t;          // 44 B on the wire

typedef struct __attribute__((packed)) {
  char     magic[4];
  uint8_t  type;            // PIG_MSG_OTA_DATA (0x26)
  uint8_t  session;
  uint16_t seq;              // chunk index, 0-based
  uint8_t  len;              // payload bytes in data[] (1..PIG_OTA_CHUNK)
  uint8_t  data[PIG_OTA_CHUNK];
} pig_ota_data_t;           // send offsetof(pig_ota_data_t, data) + len bytes

typedef struct __attribute__((packed)) {
  char     magic[4];
  uint8_t  type;            // PIG_MSG_OTA_ACK (0x27)
  uint8_t  session;
  uint16_t next;             // next chunk index the node expects (= chunks written)
  uint8_t  status;           // PigOtaStatus
} pig_ota_ack_t;             // 9 B on the wire

static_assert(sizeof(pig_ota_begin_t) == 44, "pig_ota_begin_t must be 44B");
static_assert(sizeof(pig_ota_ack_t)   == 9,  "pig_ota_ack_t must be 9B");
static_assert(sizeof(pig_ota_data_t)  <= 250, "pig_ota_data_t must fit one ESP-NOW frame");

// ================================================================
//  Board-type identification -- every Piglet variant runs on the same
//  ESP32-C5 silicon, so the ESP image header's chip_id alone can't tell
//  a PigletNode image apart from a TDongleC5_Piglet image the way it
//  could for olosnoks/warhog's mixed C5/C3 node fleet. This 1-byte code
//  is appended after the existing PIGLET_MARKER text in CORE_REQUEST/
//  HEARTBEAT messages so a Core can match a connected node to the right
//  /Firmware/<Board>.ota.bin image before ever starting a push -- see
//  coreOtaStart() in MeshNode.cpp / the Core section of each .ino.
// ================================================================
enum PigletBoardType : uint8_t {
  PIGLET_BOARD_NODE    = 0,  // PigletNode (headless mesh node)
  PIGLET_BOARD_XIAO    = 1,  // main Piglet (XIAO ESP32-S3/C5/C6/C3)
  PIGLET_BOARD_TDONGLE = 2,  // TDongleC5_Piglet
  PIGLET_BOARD_UNKNOWN = 0xFF // sender didn't include a board-type byte (pre-v3.0 Piglet node)
};
