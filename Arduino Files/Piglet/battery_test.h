#pragma once

// Battery endurance test -- writes elapsed seconds to /battery_test.csv so a
// run can be timed from the SD card after the fact (e.g. to measure runtime
// on a given battery). Gated entirely at runtime by the `batteryTest` config
// key (see wardriver.cfg): batteryTestInit() is only called from setup() when
// cfg.batteryTest is true, and batteryTestTick() is a no-op whenever the test
// file was never opened -- so both can always be compiled in and called
// unconditionally from setup()/loop() without any code changes needed to
// turn the feature on or off.

void batteryTestInit();   // Call once in setup() after SD is initialized
void batteryTestTick();   // Call every loop() iteration - writes once per second
