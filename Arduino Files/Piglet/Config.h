#pragma once
#include <Arduino.h>
#include "PinMapDefs.h"

struct Config {
  String wigleBasicToken;
  String homeSsid;
  String homePsk;
  String wardriverSsid = "Piglet-WARDRIVE";
  String wardriverPsk  = "wardrive1234";
  uint32_t gpsBaud     = 9600;
  String scanMode      = "aggressive"; // aggressive | powersaving
  String board = "auto"; // auto | s3 | c5 | c6 | exp  (pins selected at boot; reboot required after change)
  String speedUnits  = "kmh"; // kmh | mph
  int battPin        = -1;    // GPIO for battery voltage ADC (-1 = disabled). Expects 1:2 voltage divider from LiPo.
  
  // WiGLE API quota management: WiGLE allows 25 API calls/day per token.
  // Each CSV upload = 1 call. History API call is cached for 24h (only 1 call/day).
  // IMPORTANT: Requires PSRAM enabled in Arduino IDE for reliable TLS connections.
  // Default: 25 (upload all files at boot). Set to 0 to disable auto-upload.
  int maxBootUploads = 25;
};

const PinMap& detectPinsByChip();
PinMap pickPinsFromConfig();
bool wardriverIsC5();

String trimCopy(String s);
bool parseKeyValueLine(const String& lineIn, String& keyOut, String& valOut);
void cfgAssignKV(const String& k, const String& v);
bool loadConfigFromSD();
bool saveConfigToSD();
