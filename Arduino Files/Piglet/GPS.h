#pragma once
#include <Arduino.h>

// Heading smoothing constants
extern const uint32_t HEADING_HOLD_MS;
extern const float    HEADING_MIN_SPEED_KMPH;

// Held-heading state (used by Display)
extern double   lastGoodHeadingDeg;
extern uint32_t lastGoodHeadingMs;

// Heading smoothing
void   headingFeed(double deg);
double headingSmoothedDeg();

// Time helpers
String iso8601NowUTC();
time_t makeUtcEpochFromTm(struct tm* t);
