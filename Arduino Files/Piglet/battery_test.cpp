#include <Arduino.h>
#include <SD.h>
#include "battery_test.h"

static File batteryTestFile;
static uint32_t batteryTestStartMs = 0;
static uint32_t lastWriteSeconds = 0;

void batteryTestInit() {
  batteryTestStartMs = millis();
  lastWriteSeconds = 0;

  // Open CSV for writing (create or overwrite)
  if (SD.exists("/battery_test.csv")) {
    SD.remove("/battery_test.csv");
  }

  batteryTestFile = SD.open("/battery_test.csv", FILE_WRITE);
  if (!batteryTestFile) {
    Serial.println("[BATTERY_TEST] Failed to open /battery_test.csv");
    return;
  }

  // Write CSV header
  batteryTestFile.println("elapsed_seconds");
  batteryTestFile.flush();

  Serial.println("[BATTERY_TEST] Started - writing to /battery_test.csv");
}

void batteryTestTick() {
  if (!batteryTestFile) return;

  uint32_t elapsedMs = millis() - batteryTestStartMs;
  uint32_t elapsedSeconds = elapsedMs / 1000;

  // Write once per second
  if (elapsedSeconds > lastWriteSeconds) {
    lastWriteSeconds = elapsedSeconds;

    batteryTestFile.println(elapsedSeconds);
    batteryTestFile.flush();  // Force write immediately so data survives power loss

    // Optional: print every 60 seconds to reduce serial spam
    if (elapsedSeconds % 60 == 0) {
      Serial.print("[BATTERY_TEST] ");
      Serial.print(elapsedSeconds);
      Serial.println(" seconds");
    }
  }
}
