#pragma once
#include <Arduino.h>

bool   openLogFile();
void   closeLogFile();
void   appendWigleRow(const String& mac, const String& ssid, const String& auth,
                      const String& firstSeen, int channel, int rssi,
                      double lat, double lon, double altM, double accM);

String normalizeSdPath(const char* dir, const char* nameIn);
String pathBasename(const String& p);
bool   isAllowedDataPath(const String& p);
bool   moveToUploaded(const String& srcPath);

// Negotiates the fastest working SD-over-SPI clock: tries a descending
// ladder of speeds (capped by cfg.sdMaxSpiHz) via SD.begin(csPin, SPI, hz)
// and stops at the first one that mounts successfully. Assumes SPI.begin()
// has already been called with the correct pins. Logs the negotiated speed.
bool sdBeginBestClock(uint8_t csPin);

// Attempts to recover a wedged SD/SPI bus after a transient failure (e.g.
// SD.exists()/SD.open() unexpectedly failing on a file that should be
// present, typically after sustained heavy SD I/O). Fully re-mounts the
// card via SD.end() + sdBeginBestClock() and updates the global sdOk flag.
// Rate-limited internally (~4s cooldown) so a genuinely dead card doesn't
// get hammered with a remount attempt on every single file. Returns true
// if the card responded again after recovery.
bool sdTryRecover();
