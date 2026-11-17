// ================================================================
//  PigWorld - scrolling-world animation for Piglet's 128x64 two-colour
//  SSD1306. See PigWorld.h for how it plugs in.
//
//  Screen layout:
//    rows  0-15  yellow band: the sky. Sun or moon on an arc by local
//                solar time, stars at night, fireworks bursting, and
//                the title in a small cleared box on top
//    rows 16-63  blue area:   the world, 128x48, one sprite pixel per
//                screen pixel. Things that rise out of it (rockets,
//                hearts) turn yellow as they cross into the band.
//
//  World y runs 0-47 down the blue area; the band is world y -16..-1.
//
//  Sprites come from pig_sprites.h (generated, do not edit by hand).
// ================================================================
#include "PigWorld.h"
#include "Globals.h"
#include "Display.h"
#include "pig_sprites.h"
#include <WiFi.h>
#include <math.h>
#include <time.h>

static bool celebrateReq = false;

void pigWorldCelebrate() { celebrateReq = true; }

// ---- world geometry ----
static const int SW = 128;           // world width
static const int SH = 48;            // world height (the blue area)
static const int WY = OLED_YELLOW_H; // world's top row on screen; the band is world y -WY..-1

static int32_t rnd(uint32_t n) { return (int32_t)(esp_random() % n); }
static float   rndf()          { return (esp_random() % 10000) / 10000.0f; }
static int32_t floorDiv(int32_t a, int32_t b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); }

// Draw rows of a 1-bit bitmap (MSB = leftmost) into the world or the band above it, clipped to the screen.
static void blit(const uint32_t* rows, int w, int h, int x, int y, bool flip, uint8_t color) {
  for (int j = 0; j < h; j++) {
    uint32_t r = rows[j];
    if (!r) continue;
    int py = y + j;
    if (py < -WY || py >= SH) continue;
    for (int i = 0; i < w; i++)
      if (r & (0x80000000u >> i)) {
        int px = x + (flip ? w - 1 - i : i);
        if (px >= 0 && px < SW) display.drawPixel(px, WY + py, color ? SSD1306_WHITE : SSD1306_BLACK);
      }
  }
}
static void safePixel(int x, int y) {
  if (x >= 0 && x < SW && y >= -WY && y < SH) display.drawPixel(x, WY + y, SSD1306_WHITE);
}
// Sky bodies stay in the band: the sun sets behind its bottom edge
static void skyPixel(int x, int y) {
  if (x >= 0 && x < SW && y >= -WY && y < 0) display.drawPixel(x, WY + y, SSD1306_WHITE);
}
static uint32_t groundHash(uint32_t n) {
  n = (n ^ 61 ^ (n >> 16)) * 9u;
  n ^= n >> 4;
  n *= 0x27d4eb2du;
  return n ^ (n >> 15);
}

// ---- sky: sun by day, moon and stars by night, on an arc across the band ----
// Local solar time needs no time-zone setting: UTC from GPS time plus 1 h per 15 deg of longitude.
struct Sky { float hour, moon; bool night; };   // moon: phase 0 new, 0.5 full
static bool  haveLng = false;
static float lastLng = 0;

// UTC seconds from the GPS date and time, carried forward on millis() between fixes; 0 until GPS time is known.
static time_t gpsUtcNow() {
  static time_t   base   = 0;
  static uint32_t baseMs = 0;
  static uint32_t lastAge = 0xFFFFFFFFu;
  if (gps.date.isValid() && gps.time.isValid() && gps.date.year() >= 2020) {
    uint32_t age = gps.time.age();
    if (age < lastAge || !base) {
      int y = gps.date.year(), m = gps.date.month(), d = gps.date.day();
      y -= m <= 2;
      int era = y / 400, yoe = y - era * 400;
      int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
      int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
      int32_t days = era * 146097 + doe - 719468;
      base   = (time_t)days * 86400 + gps.time.hour() * 3600 + gps.time.minute() * 60 + gps.time.second();
      baseMs = millis() - age;
    }
    lastAge = age;
  }
  return base ? base + (time_t)((millis() - baseMs) / 1000) : 0;
}

static Sky skyNow() {
  if (gpsHasFix && gps.location.isValid()) { lastLng = gps.location.lng(); haveLng = true; }
  time_t now = gpsUtcNow();
  static float bootHour = -1.0f;
  static float bootMoon = 0.5f;
  if (bootHour < 0.0f) {
    bootHour = rnd(240) / 10.0f;                    // random solar time until GPS time locks
    bootMoon = rndf();
  }
  Sky s = {bootHour, bootMoon, bootHour < 6.0f || bootHour >= 18.0f};
  if (now < 1600000000 || !haveLng) return s;
  s.hour  = fmodf((now % 86400) / 3600.0f + lastLng / 15.0f + 48.0f, 24.0f);
  s.moon  = (float)fmod((now / 86400.0 - 10962.76) / 29.530588853, 1.0);   // from the new moon of 2000-01-06 18:14 UTC
  s.night = s.hour < 6 || s.hour >= 18;
  return s;
}

// p runs 0..1 across the band; the body rises at the bottom-left edge, peaks at row 5, sets bottom-right
static void arcPos(float p, int& cx, int& cy) {
  cx = (int)lroundf(-4 + p * (SW + 8));
  cy = (int)lroundf(-1 - 10 * sinf(p * 3.14159f));
}

static void drawSun(int cx, int cy, uint32_t t) {
  for (int dy = -2; dy <= 2; dy++)
    for (int dx = -2; dx <= 2; dx++)
      if (dx * dx + dy * dy <= 6) skyPixel(cx + dx, cy + dy);
  bool a = (t / 12) & 1;                             // the rays take turns to shine
  for (int k = -1; k <= 1; k += 2) {
    skyPixel(cx + 4 * k, cy); skyPixel(cx, cy + 4 * k);
    skyPixel(cx + 3 * k, cy + 3 * k); skyPixel(cx + 3 * k, cy - 3 * k);
    if (a) { skyPixel(cx + 5 * k, cy); skyPixel(cx, cy + 5 * k); }
    else   { skyPixel(cx + 4 * k, cy + 4 * k); skyPixel(cx + 4 * k, cy - 4 * k); }
  }
}

// Lit part from the phase (waxing lights the right side); the dark limb is dotted so a new moon still shows
static void drawMoon(int cx, int cy, float phase) {
  float c = cosf(phase * 6.2832f);
  for (int dy = -3; dy <= 3; dy++) {
    float w = sqrtf(12.5f - dy * dy);
    for (int dx = -3; dx <= 3; dx++) {
      if (dx * dx + dy * dy > 12) continue;
      float xs = dx / w;
      bool lit = phase < 0.5f ? xs > c : xs < -c;
      bool rim = (dx - 1) * (dx - 1) + dy * dy > 12 || (dx + 1) * (dx + 1) + dy * dy > 12 ||
                 dx * dx + (dy - 1) * (dy - 1) > 12 || dx * dx + (dy + 1) * (dy + 1) > 12;
      if (lit || (rim && ((dx + dy) & 1))) skyPixel(cx + dx, cy + dy);
    }
  }
}

static void drawStars(uint32_t t, int mx, int my) {
  uint32_t drift = millis() / 60000;                 // the sky turns 1 px a minute (burn-in)
  for (int i = 0; i < 18; i++) {
    uint32_t h = groundHash(i * 131 + 7);
    int x = (int)((h % SW + drift) % SW), y = -WY + (int)((h >> 8) % 14);
    if (abs(x - mx) < 6 && abs(y - my) < 6) continue;  // not in front of the moon
    uint32_t tw = (t + (h >> 12)) % (60 + (h >> 20) % 60);
    if (tw < 3) continue;                            // twinkle
    skyPixel(x, y);
    if ((h >> 16) % 4 == 0 && tw > 20 && tw < 25) {  // a bright one flares now and then
      skyPixel(x - 1, y); skyPixel(x + 1, y); skyPixel(x, y - 1); skyPixel(x, y + 1);
    }
  }
}

static void drawSky(const Sky& s, uint32_t t) {
  if (!s.night) {
    int cx, cy;
    arcPos((s.hour - 6) / 12, cx, cy);
    drawSun(cx, cy, t);
    return;
  }
  int mx, my;
  arcPos(fmodf(s.hour + 6, 24) / 12, mx, my);        // 18:00 -> 0, 06:00 -> 1
  drawStars(t, mx, my);
  drawMoon(mx, my, s.moon);
}

// Fast sine approximation for terrain synthesis on soft-float RISC-V (ESP32-C5/C6)
static float fastSin(float x) {
  x *= 0.15915494f;  // 1 / (2 * PI)
  x -= (int32_t)x;
  if (x < 0.0f) x += 1.0f;
  if (x > 0.5f) x -= 1.0f;
  float ax = x < 0.0f ? -x : x;
  float y  = x * (8.0f - 16.0f * ax);
  float ay = y < 0.0f ? -y : y;
  return 0.775f * y + 0.225f * y * ay;
}

// ---- far hills: a dotted ridge at a quarter of the ground speed, for depth ----
static int farHill(int32_t fx) {
  float v = 7 + 4.5f * fastSin(fx * 0.031f) + 2.5f * fastSin(fx * 0.083f + 1.3f) + 1.2f * fastSin(fx * 0.17f + 0.5f);
  return v < 0 ? 0 : (int)v;
}

// ---- clouds: 1-3, each with its own shape, height, wind and depth (parallax) ----
struct CloudShape { const uint32_t* rows; uint8_t w, h; };
static const CloudShape CLOUD_SHAPES[5] = {
  {CLOUD1, CLOUD1_W, CLOUD1_H}, {CLOUD2, CLOUD2_W, CLOUD2_H}, {CLOUD3, CLOUD3_W, CLOUD3_H},
  {CLOUD4, CLOUD4_W, CLOUD4_H}, {CLOUD5, CLOUD5_W, CLOUD5_H},
};
struct Cloud { uint8_t shape; float x; uint8_t y; float depth, wind; };
static Cloud   clouds[3];
static uint8_t cloudCount = 0;

static void newCloud(Cloud& c, int side) {
  c.shape = rnd(5);
  int w = CLOUD_SHAPES[c.shape].w;
  c.x = side == 0 ? rnd(SW + 20) - 10 : side < 0 ? -w - rnd(90) : SW + rnd(90);
  c.y = 1 + rnd(16);
  c.depth = 0.12f + rndf() * 0.25f;
  c.wind  = 0.02f + rndf() * 0.05f;
}
static void cloudsTick(int scrollDir) {
  if (!cloudCount) {
    cloudCount = 1 + rnd(3);
    for (int i = 0; i < cloudCount; i++) newCloud(clouds[i], 0);
  }
  for (int i = 0; i < cloudCount; i++) {
    Cloud& c = clouds[i];
    const CloudShape& s = CLOUD_SHAPES[c.shape];
    c.x -= c.wind + scrollDir * c.depth;
    if (c.x < -s.w - 2) newCloud(c, 1);
    else if (c.x > SW + 2) newCloud(c, -1);
    blit(s.rows, s.w, s.h, (int)lroundf(c.x), c.y, false, 1);
  }
}

// ---- birds: every 20-60 s one bird, or a loose V of 2-3 ----
struct Bird { float x, y, vx; uint8_t ph, glide; uint16_t age; };
static Bird    birds[3];
static uint8_t birdCount = 0;
static int32_t birdWait  = 400;

static void birdsTick(int scrollDir) {
  if (!birdCount) {
    if (--birdWait > 0) return;
    bool fromLeft = esp_random() & 1;
    int  dir = fromLeft ? 1 : -1;
    float vx = dir * (1.2f + rndf() * 0.6f);
    int  y = 2 + rnd(12);
    birdCount = rnd(10) < 7 ? 1 : 2 + rnd(2);
    for (int i = 0; i < birdCount; i++)
      birds[i] = {(fromLeft ? -8.0f : (float)SW + 1) - dir * i * 9, (float)(y + i * 3 - (i > 1 ? 5 : 0)), vx,
                  (uint8_t)rnd(8), 0, 0};
  }
  static const uint32_t* const FLAP[4] = {BIRD_UP, BIRD_MID, BIRD_DOWN, BIRD_MID};
  bool gone = true;
  for (int i = 0; i < birdCount; i++) {
    Bird& b = birds[i];
    b.age++;
    b.x += b.vx - scrollDir * 0.4f;
    if (b.glide) b.glide--;
    else if (rnd(100) < 2) b.glide = 10 + rnd(10);
    const uint32_t* g = b.glide ? BIRD_MID : FLAP[((b.age + b.ph) >> 1) & 3];
    blit(g, BIRD_UP_W, BIRD_UP_H, (int)lroundf(b.x), (int)lroundf(b.y + sinf(b.age * 0.15f + b.ph)), false, 1);
    if (b.x > -10 && b.x < SW + 10) gone = false;
  }
  if (gone) { birdCount = 0; birdWait = 400 + rnd(800); }
}

// ---- ground: rolls between rows 40 and 46 of the world; trees every 2000 px ----
static const int GROUND_BASE = 44;
static const int TREE_CELL   = 2000;

static int hill(int32_t wx) {
  int h = (int)lroundf(2.4f * fastSin(wx * 0.07f) + 1.2f * fastSin(wx * 0.19f + 1.0f));
  return h < -4 ? -4 : h > 2 ? 2 : h;
}
static int groundAt(int sx, int32_t gx) { return GROUND_BASE + hill(sx + gx); }

struct TreeShape { const uint32_t* rows; uint8_t w, h; };
static const TreeShape TREE_SHAPES[5] = {
  {OAK, OAK_W, OAK_H}, {PINE, PINE_W, PINE_H}, {BIRCH, BIRCH_W, BIRCH_H},
  {APPLE, APPLE_W, APPLE_H}, {BARE, BARE_W, BARE_H},
};
struct Tree { int32_t wx; const uint32_t* rows; uint8_t w, h; };
static void treeIn(int32_t cell, Tree& tr) {
  uint32_t h = groundHash((uint32_t)(cell * 7 + 12345));
  const TreeShape& s = TREE_SHAPES[(h >> 4) % 5];
  tr = {cell * TREE_CELL + (int32_t)((h >> 8) % 200), s.rows, s.w, s.h};
}
static bool treeBetween(int32_t a, int32_t b) {
  Tree tr;
  for (int32_t c = floorDiv(a - 32, TREE_CELL); c <= floorDiv(b, TREE_CELL); c++) {
    treeIn(c, tr);
    if (tr.wx <= b && tr.wx + tr.w >= a) return true;
  }
  return false;
}

struct DigSite { bool active; int32_t hx, mx; uint8_t mound; };
static DigSite site = {false, 0, 0, 0};

static void drawWorld(int32_t gx, int scrollDir) {
  // far ridge first, dotted every third column, only where it stands clear of the near ground
  int32_t fx0 = floorDiv(gx, 4);
  for (int sx = 0; sx < SW; sx++) {
    int32_t fx = sx + fx0;
    if (((fx % 3) + 3) % 3) continue;
    int y = GROUND_BASE - 6 - farHill(fx);
    if (y < groundAt(sx, gx) - 2) safePixel(sx, y);
  }

  cloudsTick(scrollDir);
  birdsTick(scrollDir);

  Tree tr;
  for (int32_t c = floorDiv(gx - 40, TREE_CELL); c <= floorDiv(gx + SW, TREE_CELL); c++) {
    treeIn(c, tr);
    int sx = tr.wx - gx;
    blit(tr.rows, tr.w, tr.h, sx, groundAt(sx + tr.w / 2, gx) - tr.h + 1, false, 1);
  }

  int py = groundAt(-1, gx);
  for (int sx = 0; sx < SW; sx++) {
    int32_t  wx = sx + gx;
    uint32_t h  = groundHash((uint32_t)(wx + 100000));
    int gy = groundAt(sx, gx);
    if (site.active && wx >= site.hx && wx < site.hx + 7) {        // dug hole
      bool edge = (wx == site.hx || wx == site.hx + 6);
      safePixel(sx, gy + (edge ? 1 : 2));
      if (!edge) safePixel(sx, gy + 3);
      py = gy;
      continue;
    }
    safePixel(sx, gy);
    for (int y = min(gy, py) + 1; y < max(gy, py); y++) safePixel(sx, y);
    if (h % 7 == 0)  safePixel(sx, gy + 1 + (h >> 3) % 2);
    if (h % 29 == 0) { safePixel(sx, gy - 1); safePixel(sx + 1, gy - 2); safePixel(sx + 2, gy - 1); }
    if (h % 53 == 0) { safePixel(sx, gy - 1); safePixel(sx, gy - 2); safePixel(sx + 1, gy - 3); safePixel(sx - 1, gy - 3); }
    py = gy;
  }

  if (site.active && site.mound) {
    static const uint32_t* const M[4] = {nullptr, MOUND1, MOUND2, MOUND3};
    static const uint8_t MW[4] = {0, MOUND1_W, MOUND2_W, MOUND3_W};
    int sx = site.mx - gx;
    blit(M[site.mound], MW[site.mound], 3, sx, groundAt(sx + 4, gx) - 3, false, 1);
  }
  if (site.active && (site.hx - gx < -40 || site.hx - gx > SW + 40)) site.active = false;
}

// ---- butterfly: every 10-40 s, bobbing on two sine waves, behind the pig ----
struct Butterfly { bool active; float x, vx, y0, ph; uint16_t age; };
static Butterfly bfly = {false, 0, 0, 0, 0, 0};
static int32_t   bflyWait = 200;

static void butterflyTick(int scrollDir) {
  if (!bfly.active) {
    if (--bflyWait > 0) return;
    bool fromLeft = esp_random() & 1;
    bfly = {true, fromLeft ? -12.0f : (float)SW + 1, (fromLeft ? 1 : -1) * (0.45f + rndf() * 0.35f),
            8.0f + rnd(22), rndf() * 6.28f, 0};
  }
  bfly.age++;
  bfly.x  += bfly.vx - scrollDir * 0.5f;
  bfly.ph += 0.25f;
  float fy = bfly.y0 + sinf(bfly.ph) * 3 + sinf(bfly.ph * 0.37f) * 4;
  int by = (int)lroundf(fy < 1 ? 1 : fy > 30 ? 30 : fy);
  static const uint32_t* const FLAP[4] = {BFLY_OPEN, BFLY_HALF, BFLY_SHUT, BFLY_HALF};
  blit(FLAP[(bfly.age >> 1) & 3], BFLY_OPEN_W, BFLY_OPEN_H, (int)lroundf(bfly.x), by, false, 1);
  if (bfly.x < -14 || bfly.x > SW + 8) { bfly.active = false; bflyWait = 200 + rnd(600); }
}

// ---- fireflies: the night's butterflies. Each drifts and glows for 0.7 s every 2-6 s ----
struct Firefly { float x, y0, ph; uint8_t glow; uint8_t wait; };
static Firefly flies[4];
static bool    fliesReady = false;

static void firefliesTick(int scrollDir, int32_t gx) {
  if (!fliesReady) {
    for (Firefly& f : flies) f = {(float)rnd(SW), 20.0f + rnd(14), rndf() * 6.28f, 0, (uint8_t)(1 + rnd(80))};
    fliesReady = true;
  }
  for (Firefly& f : flies) {
    f.ph += 0.05f;
    f.x  += sinf(f.ph * 1.3f) * 0.35f - scrollDir * 0.6f;
    if (f.x < -2) f.x += SW + 4; else if (f.x > SW + 2) f.x -= SW + 4;
    if (!f.glow) { if (--f.wait == 0) { f.glow = 14; f.wait = 40 + rnd(80); } continue; }
    f.glow--;
    int x = (int)lroundf(f.x), y = (int)lroundf(f.y0 + sinf(f.ph) * 4);
    int top = groundAt(x, gx) - 3;
    if (y > top) y = top;
    safePixel(x, y);
    if (f.glow >= 4 && f.glow < 10) { safePixel(x - 1, y); safePixel(x + 1, y); safePixel(x, y - 1); safePixel(x, y + 1); }
  }
}

// ---- truffle hunt (180 frames, 9 s) ----
static const int DIG_LEN = 180;
static const uint8_t DIG_POP[12]   = {2, 5, 7, 9, 10, 10, 9, 7, 5, 3, 1, 0};
static const uint8_t DIG_BOUNCE[4] = {1, 2, 1, 0};
static const uint8_t DIG_HOP[6]    = {0, 2, 4, 5, 4, 2};

struct DigStep {
  uint8_t frame, truffle, lift, reveal, dy, mound;
  char    mark;
  bool    dirt, hole, sparkle, crumbs, heart;
};
static DigStep digAt(int k) {
  DigStep d = {};
  d.frame = PF_STAND;
  if (k < 6) return d;
  if (k < 22) { d.frame = (((k - 6) >> 2) & 1) ? PF_SNIFF2 : PF_SNIFF1; if (k >= 14) d.mark = '?'; return d; }
  if (k < 28) { d.mark = '!'; return d; }
  if (k < 76) {
    d.frame = ((k >> 1) & 1) ? PF_SNIFF2 : PF_SNIFF1;
    d.dirt  = (k % 2 == 0);
    d.hole  = (k == 30);
    d.mound = k == 40 ? 1 : k == 54 ? 2 : k == 68 ? 3 : 0;
    return d;
  }
  if (k < 88)  { d.truffle = 1; d.reveal = 1 + (k - 76) / 3; return d; }
  if (k < 100) { d.truffle = 1; d.lift = DIG_POP[k - 88]; d.sparkle = true; d.mark = '!'; return d; }
  if (k < 112) {
    int j = k - 100;
    d.frame = PF_HAPPY; d.truffle = 1;
    d.lift = j < 4 ? DIG_BOUNCE[j] : 0;
    d.dy   = j < 6 ? DIG_HOP[j] : 0;
    d.sparkle = j < 8;
    return d;
  }
  if (k < 118) { d.truffle = 1; return d; }
  if (k < 154) {
    int b = (k - 118) / 12, j = (k - 118) % 12;
    d.frame   = j < 4 ? PF_SNIFF2 : j < 8 ? PF_SNIFF1 : PF_STAND;
    d.truffle = 1 + b + (j >= 2 ? 1 : 0);
    d.crumbs  = (j == 2);
    return d;
  }
  if (k < 158) { d.truffle = 4; return d; }
  if (k < 176) { d.frame = PF_HAPPY; d.heart = (k == 158); return d; }
  d.frame = PF_BLINK;
  return d;
}

// ---- particles: dirt/crumbs, sparks (fireworks, dust), floating hearts ----
struct Dirt { float x, y, vx, vy; bool live; };
static Dirt dirt[16];
static void spawnDirt(float x, float y, float vx, float vy) {
  for (Dirt& d : dirt) if (!d.live) { d = {x, y, vx, vy, true}; return; }
}
static void dirtTick(int32_t gx) {
  for (Dirt& d : dirt) {
    if (!d.live) continue;
    d.x += d.vx; d.vy += 0.2f; d.y += d.vy;
    int sx = (int)lroundf(d.x);
    if (sx < 0 || sx >= SW || d.y >= groundAt(sx, gx)) { d.live = false; continue; }
    safePixel(sx, (int)lroundf(d.y));
  }
}
struct Spark { float x, y, vx, vy, g; uint8_t life; };
#define SPARK_N 96
static Spark sparks[SPARK_N];
static void addSpark(float x, float y, float vx, float vy, float g, uint8_t life) {
  for (uint8_t i = 0; i < SPARK_N; i++) { Spark& s = sparks[i]; if (!s.life) { s = {x, y, vx, vy, g, life}; return; } }
}
static void sparksTick(int32_t gx, uint32_t t) {
  for (uint8_t i = 0; i < SPARK_N; i++) {
    Spark& s = sparks[i];
    if (!s.life) continue;
    s.x += s.vx + (s.g == 0 ? sinf((t + s.life) * 0.3f) * 0.2f : 0);
    s.vy += s.g; s.y += s.vy;
    int sx = (int)lroundf(s.x);
    if (--s.life == 0 || sx < 0 || sx >= SW || s.y >= groundAt(sx, gx)) { s.life = 0; continue; }
    if (s.life > 5 || (s.life & 1)) safePixel(sx, (int)lroundf(s.y));   // flicker out
  }
}
struct Heart { bool live; float x, y; int16_t age; };
static Heart hearts[8];
static void addHeart(float x, float y, int delay) {
  for (Heart& h : hearts) if (!h.live) { h = {true, x, y, (int16_t)-delay}; return; }
}
static void heartsTick() {
  for (Heart& h : hearts) {
    if (!h.live) continue;
    if (++h.age > 44) { h.live = false; continue; }
    if (h.age < 1) continue;
    if (h.age < 34 || h.age % 4 < 2)
      blit(HEART, HEART_W, HEART_H, (int)lroundf(h.x + sinf(h.age * 0.3f) * 1.5f), (int)lroundf(h.y - h.age / 3.0f), false, 1);
  }
}

// ---- fireworks: rockets climb out of the world and burst in the yellow band ----
struct Rocket { bool live; float x, y, vy; uint8_t kind; };
static Rocket  rockets[4];
static uint8_t showLeft = 0, showGap = 0;          // rockets still to launch in this show, frames to the next

static void launchShow(int n) { showLeft = min(showLeft + n, 12); }

// kind 0 ring, 1 willow (slow and drooping), 2 double ring
static void burst(float x, float y, uint8_t kind) {
  for (int i = 0; i < 5; i++)                          // a bright + for a moment at the centre
    addSpark(x + (i == 1) - (i == 2), y + (i == 3) - (i == 4), 0, 0, 0.001f, 3);
  int k = kind == 1 ? 18 : 16;
  for (int i = 0; i < k; i++) {
    float a = i * 6.2832f / k + rndf() * 0.15f;
    float v = kind == 1 ? 0.35f + rndf() * 0.55f : kind == 2 ? ((i & 1) ? 0.5f : 1.0f) : 0.9f;
    addSpark(x, y, cosf(a) * v, sinf(a) * v * 0.75f, kind == 1 ? 0.03f : 0.045f, kind == 1 ? 30 : 17 + rnd(6));
  }
}
static void rocketsTick(int32_t gx, uint32_t t) {
  if (showLeft && showGap-- == 0) {
    for (Rocket& r : rockets) if (!r.live) {
      int   x   = 8 + rnd(SW - 16);
      float y   = groundAt(x, gx) - 1;
      float top = -WY + 5 + rnd(6);                   // bursts in the band
      r = {true, (float)x, y, -sqrtf(2 * 0.08f * (y - top)), (uint8_t)rnd(3)};
      showLeft--;
      break;
    }
    showGap = 6 + rnd(14);
  }
  for (Rocket& r : rockets) {
    if (!r.live) continue;
    r.y += r.vy; r.vy += 0.08f;
    if (r.vy >= -0.25f) { r.live = false; burst(r.x, r.y, r.kind); continue; }
    int x = (int)lroundf(r.x + sinf(r.y * 0.4f) * 0.4f), y = (int)lroundf(r.y);
    safePixel(x, y); safePixel(x, y + 1);
    safePixel(x, y + 3 + (t & 1));                    // sputtering trail
  }
}

// ---- pig sprite ----
struct PigSet { uint8_t w, h; const uint32_t* frames; const uint32_t* masks; const uint32_t* edges; };
static const PigSet SET_L = {PIGL_W, PIGL_H, &PIGL_FRAMES[0][0][0], &PIGL_MASKS[0][0], &PIGL_EDGES[0][0]};
static const uint8_t PIG_JUMP = 13;

static void drawPig(const PigSet& s, uint8_t f, int x, int y, bool flip, uint8_t look) {
  blit(s.masks + f * s.h, s.w, s.h, x, y, flip, 0);
  blit(s.frames + (look * PF_COUNT + f) * s.h, s.w, s.h, x, y, flip, 1);
}

// Look: a hash of the device's address picks its spot pattern
static uint8_t lookHash(const uint8_t* mac) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < 6; i++) h = (h ^ mac[i]) * 16777619u;
  return h % PIG_LOOKS;
}

static bool hopAt(int k, int jump, uint8_t& f, int& dy) {
  dy = 0;
  if (k < 2)  { f = PF_CROUCH; return true; }
  if (k < 14) { int i = k - 2; f = PF_JUMP; dy = (int)lroundf(i * (11 - i) / 30.25f * jump); return true; }
  if (k < 16) { f = PF_LAND; return true; }
  return false;
}
static bool partyAt(int k, int jump, uint8_t& f, int& dy, bool& hearts) {
  hearts = false; dy = 0;
  if (k < 20) { if (!hopAt(k, jump, f, dy)) f = PF_HAPPY; return true; }
  if (k < 38) { f = PF_HAPPY; hearts = (k == 20); return true; }
  if (k < 44) { f = PF_STAND; return true; }
  return false;
}

enum PigAct : uint8_t { PA_WALK, PA_SNIFF, PA_TURN, PA_DIG };

// ---- the band's title, in a cleared box so the sky passes behind ----
static void drawHud(const char* title) {
  display.setTextSize(1);
  int w = (int)strlen(title) * 6;
  display.fillRect(0, 0, w + 1, 9, SSD1306_BLACK);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(title);
}

static void worldFrame() {
  static uint32_t t = 0;
  static int      x = 24, dir = 1;
  static int32_t  gx = 0;
  static bool     worldSeeded = false;
  static uint8_t  act = PA_WALK;
  static uint16_t actT = 0, walkT = 0;
  static uint32_t nextSniff = 0, nextTurn = 0, nextDig = 0;
  static int      pigY = GROUND_BASE - PIGL_H;
  static bool     partyOn = false;
  static uint16_t partyK = 0;
  static uint8_t  gallop = 0, ownLook = 0;

  const PigSet& P = SET_L;
  const int W = P.w;

  if (!worldSeeded) {
    worldSeeded = true;
    gx  = rnd(100000);
    dir = (esp_random() & 1) ? 1 : -1;
    x   = dir > 0 ? 24 : SW - W - 24;
    nextSniff = 60 + rnd(80);
    nextTurn  = 180 + rnd(200);
    nextDig   = 240 + rnd(360);
    uint8_t mac[6] = {0};
    WiFi.macAddress(mac);
    ownLook = lookHash(mac);
  }

  if (celebrateReq) { celebrateReq = false; launchShow(6); partyOn = true; partyK = 0; act = PA_WALK; }

  // ---- speed from GPS: trot on foot, gallop in a car (level 1 from 12 km/h, 2 from 50, with some hysteresis) ----
  {
    float kmh = (gpsHasFix && gps.speed.isValid()) ? gps.speed.kmph() : 0;
    uint8_t g = kmh >= 50 ? 2 : kmh >= 12 ? 1 : 0;
    if (g < gallop && kmh > (gallop == 2 ? 40 : 8)) g = gallop;
    gallop = g;
  }
  t++;

  uint8_t f = PF_STAND, pf = PF_STAND;
  int dy = 0, pdy = 0, scroll = 0;
  char mark = 0;
  bool digging = false;
  DigStep ds = {};

  if (partyOn) {
    bool hrt;
    if (!partyAt(partyK++, PIG_JUMP, pf, pdy, hrt)) partyOn = false;
    else {
      f = pf; dy = pdy;
      if (hrt) addHeart(dir > 0 ? x + W - 9 : x + 3, pigY - 6, 0);
    }
  } else {
    if (act == PA_WALK) {
      if (gallop) {                                // on the road: face forward, no stopping to sniff or dig
        if (dir < 0) act = PA_TURN;
        if (t >= nextDig)   nextDig   = t + 200 + rnd(200);
        if (t >= nextTurn)  nextTurn  = t + 300 + rnd(300);
        if (t >= nextSniff) nextSniff = t + 160 + rnd(120);
      }
      else if (t >= nextDig) {
        int32_t a = gx + (dir > 0 ? x + W - 4 : x - 22), b = gx + (dir > 0 ? x + W + 22 : x + 4);
        if (treeBetween(a, b)) nextDig = t + 20;
        else { act = PA_DIG; nextDig = t + DIG_LEN + 600 + rnd(600); }
      }
      else if (t >= nextTurn)  { act = PA_TURN;  nextTurn  = t + 300 + rnd(300); }
      else if (t >= nextSniff) { act = PA_SNIFF; nextSniff = t + 160 + rnd(120); }
      actT = 0;
    }
    switch (act) {
      case PA_SNIFF:
        if      (actT < 3)  f = PF_STAND;
        else if (actT < 35) f = (((actT - 3) >> 2) & 1) ? PF_SNIFF2 : PF_SNIFF1;
        else if (actT < 39) f = PF_STAND;
        else if (actT < 41) f = PF_BLINK;
        else if (actT < 47) f = PF_STAND;
        else act = PA_WALK;
        break;
      case PA_TURN:
        if      (actT < 3)  f = PF_STAND;
        else if (actT < 7)  f = PF_TURN;
        else if (actT == 7) { f = PF_TURN; dir = -dir; }
        else if (actT < 10) f = PF_STAND;
        else act = PA_WALK;
        break;
      case PA_DIG:
        if (actT < DIG_LEN) { ds = digAt(actT); digging = true; f = ds.frame; dy = ds.dy; mark = ds.mark; }
        else act = PA_WALK;
        break;
      default: break;
    }
    if (act == PA_WALK) { walkT++; f = PF_WALK1 + ((walkT >> (gallop ? 0 : 1)) & 3); scroll = 1 + gallop; }
    else actT++;
  }
  gx += scroll * dir;
  if (!scroll && t % 1200 == 0) gx += dir;              // creep 1 px a minute while standing (burn-in)

  int tx = dir > 0 ? 24 + gallop * 20 : SW - W - 24;    // galloping, it surges ahead
  if (x != tx && scroll && t % 5 == 0) x += (tx > x) ? 1 : -1;
  int want = 99;
  for (int sx = x + 4; sx <= x + W - 6; sx++) want = min(want, groundAt(sx, gx));
  want -= P.h;
  if (abs(pigY - want) > 3) pigY = want;
  else if ((t & 1) == 0 && pigY != want) pigY += (want > pigY) ? 1 : -1;
  const int PIG_Y = pigY;

  int tsx = dir > 0 ? x + W + 1 : x - 8;
  if (digging && ds.hole) site = {true, tsx + gx, tsx + gx + (dir > 0 ? 8 : -10), 0};
  if (digging && ds.mound && site.active) site.mound = ds.mound;

  // ---- draw ----
  Sky sky = skyNow();
  display.clearDisplay();
  drawSky(sky, t);
  drawWorld(gx, scroll * dir);
  if (sky.night) firefliesTick(scroll * dir, gx);
  else           butterflyTick(scroll * dir);

  // speed lines streaming off the back of a galloping pig
  if (gallop == 2 && scroll) {
    for (int k = 0; k < 3; k++) {
      int ly = PIG_Y + 3 + k * 5, ph = (int)((t * 6 + k * 23) % 64);
      int lx = dir > 0 ? x - 3 - ph : x + W + 3 + ph, len = 6 + (k * 3 + (int)t / 4) % 7;
      if ((ph + k * 7) % 32 < 24)
        for (int i = 0; i < len; i++) safePixel(lx + (dir > 0 ? -i : i), ly);
    }
  }

  drawPig(P, f, x, PIG_Y - dy, dir < 0 && f != PF_TURN, ownLook);

  if (gallop && scroll && t % 3 == 0) {                 // dust kicked up by the back hooves
    int hx = dir > 0 ? x + 6 : x + W - 6;
    int gy = groundAt(hx, gx);
    addSpark(hx, gy - 1, -dir * (0.5f + rndf() * 0.9f), -0.4f - rndf() * 0.5f, 0.09f, 6 + rnd(5));
  }

  if (digging) {
    int gy = groundAt(tsx + 3, gx);
    int snx = dir > 0 ? x + W : x - 1;
    if (ds.dirt) for (int k = 0; k < 2; k++) spawnDirt(snx, gy - 1, dir * (0.3f + rndf() * 1.6f) * (rnd(4) ? 1 : -1), -1.2f - rndf() * 1.8f);
    if (ds.crumbs) for (int k = 0; k < 3; k++) spawnDirt(tsx + 3, gy - 4, (rndf() - 0.5f) * 2, -0.8f - rndf());
    if (ds.truffle) {
      static const uint32_t* const TRUF[5] = {nullptr, TRUFFLE, TRUFFLE_BITE1, TRUFFLE_BITE2, TRUFFLE_CRUMBS};
      const uint32_t* g = TRUF[ds.truffle];
      if (ds.reveal) blit(g, TRUFFLE_W, ds.reveal > TRUFFLE_H ? TRUFFLE_H : ds.reveal, tsx, gy + 2 - ds.reveal, false, 1);
      else           blit(g, TRUFFLE_W, TRUFFLE_H, tsx, gy - 6 - ds.lift, false, 1);
      if (ds.sparkle) {
        int ty = gy - 6 - ds.lift;
        bool a = (t >> 2) & 1;
        blit(a ? SPARKLE_PLUS : SPARKLE_X, 3, 3, tsx - 4, ty - 2 + ((t >> 3) & 1), false, 1);
        blit(a ? SPARKLE_X : SPARKLE_PLUS, 3, 3, tsx + 8, ty + 1, false, 1);
      }
    }
    if (ds.heart) addHeart(dir > 0 ? x + W - 9 : x + 3, PIG_Y - dy - 6, 0);
  }
  rocketsTick(gx, t);
  dirtTick(gx);
  sparksTick(gx, t);
  heartsTick();

  // marks, in the regular 6x8 font (setCursor is the glyph's top-left)
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  if (mark == '!') {
    int hx = (dir > 0) ? x + W - 9 : x + 3;
    display.setCursor(hx + 1, WY + PIG_Y - dy - 8);
    display.print('!');
  } else if (mark == '?' && ((t >> 3) & 1) == 0) {
    display.setCursor(dir > 0 ? x + W + 1 : x - 6, WY + PIG_Y);
    display.print('?');
  }

  drawHud("PIGLET");
}

bool pigWorldTick() {
  // 20 fps. If the loop was held up, carry on from the next frame rather than racing to catch up.
  static uint32_t lastMs = 0;
  uint32_t now = millis();
  if (now - lastMs < 50) return false;
  lastMs = now;
  worldFrame();
  return true;
}
