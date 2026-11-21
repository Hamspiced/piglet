#pragma once
// Compile-time USB bench instrumentation only. No network endpoint is added.
#ifdef PIGLET_SD_READBACK_TEST
constexpr unsigned SD_AUDIT_LIMIT = 129;
struct SdAuditReceipt { uint32_t session, sequence, offset, length, crc; };
static SdAuditReceipt sdAuditRows[SD_AUDIT_LIMIT], sdAuditLast = {};
static unsigned sdAuditCount = 0, sdAuditOverflow = 0;
static bool sdAuditActive = false, sdAuditLastValid = false;
static String sdAuditPath;
static uint32_t sdAuditPendingRows = 0;
static uint32_t sdAuditCrc(const char* p, size_t n) {
  uint32_t crc = 0xffffffff;
  for (size_t i = 0; i < n; ++i) {
    crc ^= (uint8_t)p[i];
    for (unsigned b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
  }
  return crc ^ 0xffffffff;
}
#endif
