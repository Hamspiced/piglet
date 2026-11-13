#pragma once
#include <Arduino.h>
#include <vector>

// Real gzip (RFC 1952) compression for CSV uploads, using the ESP32 ROM's
// built-in miniz DEFLATE compressor (tdefl_compress/tdefl_init) -- these
// symbols live in the chip's boot ROM (confirmed present at fixed addresses
// in the linked build) and cost zero flash/IRAM; the only overhead is a
// one-time ~160KB tdefl_compressor allocation from PSRAM while compressing.
// This is real compression (unlike the zero-ratio "stored block" gzip
// framing WebUI.cpp's /downloadAll uses for browser downloads), intended to
// shrink CSVs before they go out over the air to WiGLE/WDGoWars/Wardrift.

// Compresses srcPath into a valid gzip file at dstPath. Returns false on
// any allocation, SD, or compression failure; dstPath is removed in that
// case so a half-written scratch file is never left behind.
bool gzipCompressFileToFile(const String& srcPath, const String& dstPath);

// Merges multiple WiGLE CSVs into one gzip-compressed archive: the first
// successfully-opened file's two WiGLE header lines are kept verbatim, and
// the same two lines are stripped from every subsequent file, so the
// decompressed result is one valid WiGLE CSV body covering every input
// file -- suitable for WiGLE's single-file-per-upload API accepting many
// files' worth of rows in one call. Files that fail to open are skipped.
bool gzipCompressMergedFilesToFile(const std::vector<String>& srcPaths, const String& dstPath);

// Shared CRC32 (gzip/PNG polynomial 0xEDB88320, reflected) -- also used by
// WebUI.cpp's /downloadAll so there's only one implementation in the tree.
uint32_t gzipCrc32Update(uint32_t crc, const uint8_t* data, size_t len);
