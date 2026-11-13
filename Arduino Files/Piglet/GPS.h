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

// Rejects exact Null Island (0,0 -- a real location in the Gulf of Guinea,
// virtually certain to be a GPS-less sentinel rather than a genuine fix for
// a wardriving device), any coordinate outside the physically possible
// range for latitude/longitude, and non-finite values from corrupted GPS
// data. Used everywhere a position is about to be cached or logged so a
// bad value can never end up in a CSV row.
bool gpsCoordIsValid(double lat, double lon);
