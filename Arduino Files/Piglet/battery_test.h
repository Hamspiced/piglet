#pragma once

// Battery endurance test - writes elapsed seconds to /battery_test.csv
// To enable:  #include "battery_test.h" and call batteryTestTick() in loop()
// To disable: remove the include and delete battery_test.h / battery_test.cpp

void batteryTestInit();   // Call once in setup() after SD is initialized
void batteryTestTick();   // Call every loop() iteration - writes once per second
