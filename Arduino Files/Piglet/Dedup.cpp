#include "Dedup.h"
#include "esp_heap_caps.h"

static uint64_t* seenTable = nullptr;

uint32_t seenCount = 0;
uint32_t seenCollisions = 0;

bool initSeenTable() {
  if (seenTable) return true; // already allocated

  static_assert((SEEN_TABLE_SIZE & (SEEN_TABLE_SIZE - 1)) == 0,
                "SEEN_TABLE_SIZE must be power of 2");

  const size_t bytes = (size_t)SEEN_TABLE_SIZE * sizeof(uint64_t);

  // Allocate from internal heap (more reliable for WiFi + WebServer coexistence)
  seenTable = (uint64_t*) heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!seenTable) return false;

  memset(seenTable, 0, bytes);
  return true;
}

void resetSeenTable() {
  if (!seenTable) return;
  memset(seenTable, 0, SEEN_TABLE_SIZE * sizeof(uint64_t));
  seenCount = 0;
  seenCollisions = 0;

  Serial.printf("[DEDUP] Seen-table reset (size=%lu entries, %lu bytes)\n",
                (unsigned long)SEEN_TABLE_SIZE,
                (unsigned long)(SEEN_TABLE_SIZE * sizeof(uint64_t)));
}

// Mix 48-bit key into a hash index (32-bit), then mask into table.
static inline uint32_t hash48(uint64_t k) {
  // xorshift-style mixing
  k ^= (k >> 33);
  k *= 0xff51afd7ed558ccdULL;
  k ^= (k >> 33);
  k *= 0xc4ceb9fe1a85ec53ULL;
  k ^= (k >> 33);
  return (uint32_t)k;
}

// Parse "AA:BB:CC:DD:EE:FF" into a 48-bit key.
// Returns 0 if parsing fails (we treat 0 as "invalid").
uint64_t bssidStrToKey48(const String& mac) {
  // Expected length 17, but we'll be lenient.
  uint64_t v = 0;
  int nibs = 0;

  for (int i = 0; i < mac.length(); i++) {
    char c = mac[i];
    int val = -1;

    if (c >= '0' && c <= '9') val = c - '0';
    else if (c >= 'a' && c <= 'f') val = 10 + (c - 'a');
    else if (c >= 'A' && c <= 'F') val = 10 + (c - 'A');
    else continue; // skip ':' or any separators

    v = (v << 4) | (uint64_t)val;
    nibs++;
    if (nibs >= 12) break; // 12 hex nibbles = 48 bits
  }

  if (nibs != 12) return 0;
  return v; // 48-bit in low bits
}

// Check if we've seen key; if not, insert it.
// Returns true if already present; false if inserted now.
// Note: uses 0 as empty, so we store (key|1) to avoid 0.
bool seenCheckOrInsert(uint64_t key48) {
  if (key48 == 0) return false; // treat invalid parse as "not seen"

  uint64_t stored = (key48 | 1ULL); // never 0
  uint32_t idx = hash48(stored) & (SEEN_TABLE_SIZE - 1);

  for (uint32_t probe = 0; probe < SEEN_TABLE_SIZE; probe++) {
    uint32_t p = (idx + probe) & (SEEN_TABLE_SIZE - 1);
    uint64_t slot = seenTable[p];

    if (slot == 0) {
      // empty -> insert
      seenTable[p] = stored;
      seenCount++;
      if (probe) seenCollisions++;
      return false; // wasn't present
    }
    if (slot == stored) {
      return true;  // already seen
    }
  }

  // Table full: fail "open" (treat as already-seen to stop growth)
  return false;
}
