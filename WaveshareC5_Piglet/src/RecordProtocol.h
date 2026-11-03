#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Local record extension. ACK follows a full CSV row write and successful
// fflush + fsync. SD power-loss behavior still requires hardware testing;
// duplicate state is in RAM, not a durable exactly-once transaction log.
namespace RecordProtocol {
constexpr uint8_t DATA = 115, ACK = 116;
constexpr uint8_t MAX_ATTEMPTS = 3;
constexpr uint32_t ACK_WAIT_MS = 150;
constexpr size_t TEXT_MAX = 200;
struct __attribute__((packed)) Packet {
  char magic[4]; uint8_t type;
  uint32_t session, sequence;
  uint16_t len;
  char text[TEXT_MAX + 1];
};
static_assert(sizeof(Packet) == 216, "Record packet wire size changed");
inline uint32_t requestSession(const char* text, uint16_t len) {
  if (len != 14 || memcmp(text, "CTRL2:", 6)) return 0;
  uint32_t value = 0;
  for (uint8_t i = 6; i < 14; ++i) {
    char c = text[i]; uint8_t digit;
    if (c >= '0' && c <= '9') digit = c - '0';
    else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
    else return 0;
    value = (value << 4) | digit;
  }
  return value;
}
}
