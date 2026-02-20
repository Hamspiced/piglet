#pragma once
#include <Arduino.h>
#include <stdint.h>

static constexpr uint32_t SEEN_TABLE_SIZE = 8192;   // 64KB
// or 4096 for 32KB if you want extra safety

extern uint32_t seenCount;
extern uint32_t seenCollisions;

bool     initSeenTable();
void     resetSeenTable();
uint64_t bssidStrToKey48(const String& mac);
bool     seenCheckOrInsert(uint64_t key48);
