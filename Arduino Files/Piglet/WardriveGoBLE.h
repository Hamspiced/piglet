#pragma once
#include <Arduino.h>

// ================================================================
//  Wardrive Go / WarBLE Go  (RocketGod "iosESP") BLE bridge
// ================================================================
// Makes Piglet look like a RocketGod "iosESP" wardrive bridge so the phone
// Wardrive Go / WarBLE Go app pairs with it and plots the APs this Piglet
// (and, in mesh mode, its whole node cluster) hears, live on the map.
//
// Protocol: iosESP v1.5.
//   GATT service  47686F73-7445-5350-4272-696467655376  (primary advertisement)
//     RX   0147686F-7374-4553-5042-726964675852  write / write-NR  (phone -> board cmds)
//     TX   0247686F-7374-4553-5042-726964675854  notify            (board -> phone)
//     CTRL 0347686F-7374-4553-5042-72694C525443  write / write-NR  (also accepts cmds)
//     OTA  0447686F-7374-4553-5042-72694F544144  write-NR          (firmware image; stubbed)
//   Advertised name: "iosESP-XXXX" (last two octets of the BLE address) --
//   this is a protocol identifier the app's scanner keys off of, NOT a
//   branding string. Do not rename it.
//   Input: unframed ASCII commands are accepted (we also unwrap the 12-byte
//          GB frame if present). Output: text WD: lines wrapped in DATA frames.
//
// Scope: only ever active on the Mesh Node/Core page, gated on
// cfg.wardriveGoBridge -- see enterNodeMode()/exitNodeMode()/enterCoreMode()/
// exitCoreMode() in MeshNode.cpp.

// Lifecycle -- call from the Mesh Node/Core enter/exit hooks.
void wardriveGoBleBegin();
void wardriveGoBleStop();

// Main-loop service: drains inbound commands, streams queued WD: lines,
// emits the periodic WD:STATUS heartbeat. Call once per tick while the
// Mesh Node/Core page is active (no-op if the bridge was never begun).
void wardriveGoBleTick();

// Forward one scanned/received AP. `auth` is one of the plain tokens this
// firmware already uses (OPEN/WEP/WPA/WPA2/WPA3/WPAWPA2/WPA2EAP/WPA2WPA3),
// i.e. push BEFORE any Biscuit bracket-notation rewrite. No-op unless a
// phone is actively subscribed; drops (does not buffer) if the phone is
// connected but momentarily can't keep up -- both apps de-dup by BSSID, so
// a lossy stream of re-sightings still converges on a complete map.
void wardriveGoPushAp(const String& bssid, const String& ssid,
                      const String& auth, int channel, int rssi);

// Forward a GPS fix -> WD:GPS. `sats` = satellites in use (0 if unknown).
void wardriveGoPushGps(double lat, double lon, double altMeters, int sats);

// Report whether a GPS module is physically present (drive this from e.g.
// `gps.charsProcessed() > 0`), independent of whether it has a fix yet.
// WD:CAPS carries this as gps=0/1 and is sent once, when the phone
// subscribes -- so without this a phone that connects before the first fix
// is told gps=0 for the whole session.
void wardriveGoSetGpsPresent(bool present);

// Status accessors for the Mesh page's "WG: ..." indicator.
bool wardriveGoBridgeRunning();    // true whenever the BLE stack is up (advertising or linked)
bool wardriveGoBridgeConnected();  // true while a phone is linked (connected central)
