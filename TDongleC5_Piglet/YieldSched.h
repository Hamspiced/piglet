#pragma once
#include <math.h>
#include <stdint.h>

// ================================================================
//  YieldSched — per-channel yield tracking + ranking, trimmed down from
//  olosnoks/warhog's scheduler/yield_sched.h (host-testable, no Arduino/WiFi
//  deps). Only the two primitives the Core needs are kept: folding raw
//  per-channel hit counts into a smoothed yield EWMA (sched_on_yield), and
//  sorting a channel set by descending score (sched_rank). The donor file's
//  budget-based greedy assignment, speed-scaled dwell, Jaccard distance and
//  reassignment-hysteresis helpers aren't used here and were dropped.
// ================================================================

// Per-channel running stats. `yield` is an EWMA of unique/interesting
// records-per-second; `density` is an EWMA of gross records-per-second
// (used to discount channels that are merely noisy, not productive).
struct ChanStat {
  float    yield    = 0.0f;
  float    density  = 0.0f;
  uint32_t last_scan_ms = 0;
};

struct SchedCfg {
  float    alpha     = 0.30f;  // EWMA smoothing factor
  float    eps       = 0.05f;  // exploration floor so a never-scanned channel stays eligible
  float    starve_k  = 0.50f;  // freshness bonus weight for long-unscanned channels
  uint32_t starve_ms = 20000;  // time to reach full freshness bonus
};

static inline float sched_freshness(const ChanStat& s, uint32_t now, const SchedCfg& c) {
  uint32_t age = now - s.last_scan_ms;
  float a = (float)age / (float)c.starve_ms;
  if (a > 1.0f) a = 1.0f;
  return 1.0f + c.starve_k * a;
}

static inline float sched_band_weight(uint8_t ch) { return (ch <= 14) ? 1.15f : 1.0f; }

static inline float sched_score(const ChanStat& s, uint8_t ch, uint32_t now, const SchedCfg& c) {
  return (s.yield + c.eps) * sched_freshness(s, now, c) * sched_band_weight(ch)
         / sqrtf(1.0f + s.density);
}

// EWMA update from one yield-accounting window for channel index `idx`.
static inline void sched_on_yield(ChanStat* stat, uint8_t idx,
                                  uint8_t uniq, uint8_t gross, float dt_s,
                                  const SchedCfg& c) {
  float iy = (dt_s > 0) ? (float)uniq / dt_s : 0.0f;
  float id = (dt_s > 0) ? (float)gross / dt_s : 0.0f;
  stat[idx].yield   = c.alpha * iy + (1.0f - c.alpha) * stat[idx].yield;
  stat[idx].density = c.alpha * id + (1.0f - c.alpha) * stat[idx].density;
}

// Fill order[0..n_ch-1] with channel INDICES sorted by score (highest first).
// `stat[i]` corresponds to `channels[i]`.
static inline void sched_rank(const ChanStat* stat, const uint8_t* channels, int n_ch,
                              uint32_t now, const SchedCfg& c, uint8_t* order) {
  for (int i = 0; i < n_ch; ++i) order[i] = (uint8_t)i;
  for (int i = 0; i < n_ch; ++i)
    for (int j = i + 1; j < n_ch; ++j)
      if (sched_score(stat[order[j]], channels[order[j]], now, c) >
          sched_score(stat[order[i]], channels[order[i]], now, c)) {
        uint8_t t = order[i]; order[i] = order[j]; order[j] = t;
      }
}
