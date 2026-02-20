#include "GPS.h"
#include "Globals.h"
#include <math.h>
#include <time.h>
#include <sys/time.h>

// ---- Heading smoothing ----

static const uint8_t HEADING_AVG_N = 8; // samples in moving window (tune 5-12)
const uint32_t HEADING_HOLD_MS = 4000UL;     // keep last good heading for this long if GPS drops
const float    HEADING_MIN_SPEED_KMPH = 3.0f; // below this, heading is usually noisy/unreliable

static float headingSinBuf[8] = {0};
static float headingCosBuf[8] = {0};
static uint8_t headingIdx = 0;
static uint8_t headingCount = 0;
static float headingSinSum = 0.0f;
static float headingCosSum = 0.0f;

double   lastGoodHeadingDeg = NAN;
uint32_t lastGoodHeadingMs  = 0;

static inline double normDeg360(double deg) {
  while (deg < 0) deg += 360.0;
  while (deg >= 360.0) deg -= 360.0;
  return deg;
}

// Feed a new heading sample (deg). Updates circular moving average sums.
void headingFeed(double deg) {
  deg = normDeg360(deg);
  const float r = (float)(deg * (M_PI / 180.0));
  const float s = sinf(r);
  const float c = cosf(r);

  if (headingCount < HEADING_AVG_N) {
    headingCount++;
  } else {
    // remove outgoing sample from sums
    headingSinSum -= headingSinBuf[headingIdx];
    headingCosSum -= headingCosBuf[headingIdx];
  }

  headingSinBuf[headingIdx] = s;
  headingCosBuf[headingIdx] = c;
  headingSinSum += s;
  headingCosSum += c;

  headingIdx++;
  if (headingIdx >= HEADING_AVG_N) headingIdx = 0;
}

// Return smoothed heading in degrees; NAN if no samples yet
double headingSmoothedDeg() {
  if (headingCount == 0) return NAN;
  // atan2(y, x) where y=sin sum, x=cos sum
  double ang = atan2((double)headingSinSum, (double)headingCosSum) * (180.0 / M_PI);
  return normDeg360(ang);
}

// ---- Time helpers ----

time_t makeUtcEpochFromTm(struct tm* t) {
  // Save current TZ
  const char* oldTz = getenv("TZ");

  // Force UTC
  setenv("TZ", "UTC0", 1);
  tzset();

  time_t epoch = mktime(t);  // treated as UTC while TZ=UTC0

  // Restore TZ (or unset)
  if (oldTz) setenv("TZ", oldTz, 1);
  else unsetenv("TZ");
  tzset();

  return epoch;
}

String iso8601NowUTC() {
  // 1) Prefer GPS UTC time when fresh/valid
  if (gps.date.isValid() && gps.time.isValid() &&
      gps.date.age() < 15000 && gps.time.age() < 15000) {

    char buf[32];
    snprintf(buf, sizeof(buf),
             "%04d-%02d-%02d %02d:%02d:%02d",
             gps.date.year(), gps.date.month(), gps.date.day(),
             gps.time.hour(), gps.time.minute(), gps.time.second());
    return String(buf);
  }

  // 2) If system time is set (from GPS in your loop), use it
  time_t now = time(nullptr);
  if (now > 1700000000) { // sanity check (~2023+)
    struct tm tmUtc;
    gmtime_r(&now, &tmUtc);

    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmUtc);
    return String(buf);
  }

  // 3) Last resort: millis-based placeholder
  uint32_t s = millis() / 1000;
  char buf[32];
  snprintf(buf, sizeof(buf), "1970-01-01 00:%02lu:%02lu",
           (unsigned long)((s/60)%60), (unsigned long)(s%60));
  return String(buf);
}
