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
  bool batteryTest   = false; // Enable battery test (logs elapsed time to /battery_test.csv)
  
  // Boot auto-upload limit:
  //  -1 = upload ALL files at boot (no limit)
  //   0 = disabled (no auto-upload at boot)
  //  1+ = upload up to N files at boot (WiGLE allows 25 API calls/day)
  // IMPORTANT: Requires PSRAM enabled in Arduino IDE for reliable TLS connections.
  int maxBootUploads = 25;

  // WDGoWars API key from https://wdgwars.pl/profile -> "Generate API key".
  // If set, CSVs are uploaded to WDGoWars BEFORE WiGLE at every boot.
  // Leave empty to disable WDGoWars uploads.
  String wdgwarsApiKey;
};

const PinMap& detectPinsByChip();
PinMap pickPinsFromConfig();
bool wardriverIsC5();

String trimCopy(String s);
bool parseKeyValueLine(const String& lineIn, String& keyOut, String& valOut);
void cfgAssignKV(const String& k, const String& v);
bool loadConfigFromSD();
bool saveConfigToSD();
