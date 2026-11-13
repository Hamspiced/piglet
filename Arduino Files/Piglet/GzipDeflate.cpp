#include "GzipDeflate.h"
#include <SD.h>
#include <functional>
#include "esp_heap_caps.h"

// ---- Trimmed ROM miniz declarations ----
// The ESP32-C5 boot ROM links Espressif's miniz tdefl_* compressor at fixed
// addresses (confirmed present in this build via the linker map), already
// resolved automatically by the Arduino-ESP32 3.3.10 linker scripts'
// PROVIDE() directives -- no library to link, no flash cost. Arduino-ESP32
// doesn't bundle ESP-IDF's esp_rom/include/miniz.h though, so the exact
// struct layout/prototypes below are ported from that header (trimmed to
// just the low-level tdefl_init()/tdefl_compress() pieces actually used
// here) -- this layout MUST match the ROM's compiled version exactly, since
// we're calling directly into ROM code that reads/writes these fields.
extern "C" {

typedef unsigned char  mz_uint8;
typedef unsigned short mz_uint16;
typedef unsigned int   mz_uint32;
typedef unsigned int   mz_uint;
typedef int            mz_bool;

#define TDEFL_LESS_MEMORY 1
enum {
  TDEFL_MAX_HUFF_TABLES = 3, TDEFL_MAX_HUFF_SYMBOLS_0 = 288, TDEFL_MAX_HUFF_SYMBOLS_1 = 32,
  TDEFL_MAX_HUFF_SYMBOLS_2 = 19, TDEFL_LZ_DICT_SIZE = 32768, TDEFL_LZ_DICT_SIZE_MASK = TDEFL_LZ_DICT_SIZE - 1,
  TDEFL_MIN_MATCH_LEN = 3, TDEFL_MAX_MATCH_LEN = 258
};
#if TDEFL_LESS_MEMORY
enum {
  TDEFL_LZ_CODE_BUF_SIZE = 24 * 1024, TDEFL_OUT_BUF_SIZE = (TDEFL_LZ_CODE_BUF_SIZE * 13) / 10,
  TDEFL_MAX_HUFF_SYMBOLS = 288, TDEFL_LZ_HASH_BITS = 12, TDEFL_LEVEL1_HASH_SIZE_MASK = 4095,
  TDEFL_LZ_HASH_SHIFT = (TDEFL_LZ_HASH_BITS + 2) / 3, TDEFL_LZ_HASH_SIZE = 1 << TDEFL_LZ_HASH_BITS
};
#endif

typedef mz_bool (*tdefl_put_buf_func_ptr)(const void* pBuf, int len, void* pUser);

typedef enum {
  TDEFL_STATUS_BAD_PARAM = -2, TDEFL_STATUS_PUT_BUF_FAILED = -1, TDEFL_STATUS_OKAY = 0, TDEFL_STATUS_DONE = 1
} tdefl_status;

typedef enum {
  TDEFL_NO_FLUSH = 0, TDEFL_SYNC_FLUSH = 2, TDEFL_FULL_FLUSH = 3, TDEFL_FINISH = 4
} tdefl_flush;

typedef struct {
  tdefl_put_buf_func_ptr m_pPut_buf_func;
  void* m_pPut_buf_user;
  mz_uint m_flags, m_max_probes[2];
  int m_greedy_parsing;
  mz_uint m_adler32, m_lookahead_pos, m_lookahead_size, m_dict_size;
  mz_uint8 *m_pLZ_code_buf, *m_pLZ_flags, *m_pOutput_buf, *m_pOutput_buf_end;
  mz_uint m_num_flags_left, m_total_lz_bytes, m_lz_code_buf_dict_pos, m_bits_in, m_bit_buffer;
  mz_uint m_saved_match_dist, m_saved_match_len, m_saved_lit, m_output_flush_ofs, m_output_flush_remaining,
          m_finished, m_block_index, m_wants_to_finish;
  tdefl_status m_prev_return_status;
  const void* m_pIn_buf;
  void* m_pOut_buf;
  size_t *m_pIn_buf_size, *m_pOut_buf_size;
  tdefl_flush m_flush;
  const mz_uint8* m_pSrc;
  size_t m_src_buf_left, m_out_buf_ofs;
  mz_uint8 m_dict[TDEFL_LZ_DICT_SIZE + TDEFL_MAX_MATCH_LEN - 1];
  mz_uint16 m_huff_count[TDEFL_MAX_HUFF_TABLES][TDEFL_MAX_HUFF_SYMBOLS];
  mz_uint16 m_huff_codes[TDEFL_MAX_HUFF_TABLES][TDEFL_MAX_HUFF_SYMBOLS];
  mz_uint8 m_huff_code_sizes[TDEFL_MAX_HUFF_TABLES][TDEFL_MAX_HUFF_SYMBOLS];
  mz_uint8 m_lz_code_buf[TDEFL_LZ_CODE_BUF_SIZE];
  mz_uint16 m_next[TDEFL_LZ_DICT_SIZE];
  mz_uint16 m_hash[TDEFL_LZ_HASH_SIZE];
  mz_uint8 m_output_buf[TDEFL_OUT_BUF_SIZE];
} tdefl_compressor;

// Low 12 bits = max dictionary-search probes (higher = slower/better ratio).
// No TDEFL_WRITE_ZLIB_HEADER flag -- gzip wraps a RAW deflate stream with
// its own header/trailer, not a zlib one.
static const int TDEFL_DEFAULT_MAX_PROBES = 128;

tdefl_status tdefl_init(tdefl_compressor* d, tdefl_put_buf_func_ptr pPut_buf_func, void* pPut_buf_user, int flags);
tdefl_status tdefl_compress(tdefl_compressor* d, const void* pIn_buf, size_t* pIn_buf_size,
                             void* pOut_buf, size_t* pOut_buf_size, tdefl_flush flush);

}  // extern "C"

// ---- CRC32 (gzip/PNG polynomial, reflected) -- shared with /downloadAll ----
uint32_t gzipCrc32Update(uint32_t crc, const uint8_t* data, size_t len) {
  static const uint32_t T[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
    0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
    0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C
  };
  crc = ~crc;
  for (size_t i = 0; i < len; i++) {
    crc = (crc >> 4) ^ T[(crc ^ data[i]) & 0xF];
    crc = (crc >> 4) ^ T[(crc ^ (data[i] >> 4)) & 0xF];
  }
  return ~crc;
}

namespace {

// Accumulates gzip header/CRC32/ISIZE bookkeeping and writes compressed
// bytes straight to an open SD file as they're produced.
struct GzipSink {
  File out;
  uint32_t crc = 0;
  uint32_t isize = 0;

  bool writeHeader() {
    static const uint8_t hdr[10] = { 0x1f, 0x8b, 0x08, 0x00, 0, 0, 0, 0, 0, 0xff };
    return out.write(hdr, 10) == 10;
  }
  bool writeCompressed(const uint8_t* buf, size_t n) {
    if (!n) return true;
    return out.write(buf, n) == n;
  }
  void updateCrc(const uint8_t* raw, size_t n) {
    crc = gzipCrc32Update(crc, raw, n);
    isize += (uint32_t)n;
  }
  bool writeTrailer() {
    uint8_t t[8] = {
      (uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16), (uint8_t)(crc >> 24),
      (uint8_t)isize, (uint8_t)(isize >> 8), (uint8_t)(isize >> 16), (uint8_t)(isize >> 24)
    };
    return out.write(t, 8) == 8;
  }
};

// Drives the ROM compressor: repeatedly calls readChunk(buf, cap) to pull
// raw bytes (readChunk returns 0 when there's no more input), feeds them
// through tdefl_compress(), and writes compressed output + CRC32/ISIZE via
// sink. Allocates the (~160KB) tdefl_compressor from PSRAM for the
// duration of the call and frees it before returning.
bool compressStream(GzipSink& sink, const std::function<int(uint8_t*, size_t)>& readChunk) {
  tdefl_compressor* comp = (tdefl_compressor*)heap_caps_malloc(sizeof(tdefl_compressor), MALLOC_CAP_SPIRAM);
  if (!comp) {
    Serial.println("[GZIP] Failed to allocate tdefl_compressor from PSRAM");
    return false;
  }

  // inBuf/outBuf used to be plain stack locals (2048 + 4096 = 6KB). This
  // function runs deep inside the boot-time call chain (setup() ->
  // uploadAllPending() -> gzip*ToFile() -> here), and the cumulative stack
  // usage already built up by that point, plus these 6KB, plus whatever the
  // ROM tdefl_compress() itself uses internally, was enough to overflow the
  // default 8KB loopTask stack -- observed in the field as a RISC-V "Stack
  // protection fault" (stack pointer ended up ~5KB below the task's
  // allocated stack region). Heap/PSRAM-allocating them instead, right next
  // to the compressor, keeps this function's own stack frame tiny.
  uint8_t* inBuf  = (uint8_t*)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
  uint8_t* outBuf = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
  if (!inBuf || !outBuf) {
    Serial.println("[GZIP] Failed to allocate scratch buffers from PSRAM");
    if (inBuf)  heap_caps_free(inBuf);
    if (outBuf) heap_caps_free(outBuf);
    heap_caps_free(comp);
    return false;
  }

  if (tdefl_init(comp, nullptr, nullptr, TDEFL_DEFAULT_MAX_PROBES) != TDEFL_STATUS_OKAY) {
    Serial.println("[GZIP] tdefl_init failed");
    heap_caps_free(inBuf);
    heap_caps_free(outBuf);
    heap_caps_free(comp);
    return false;
  }

  if (!sink.writeHeader()) {
    heap_caps_free(inBuf);
    heap_caps_free(outBuf);
    heap_caps_free(comp);
    return false;
  }

  bool ok = true;
  int n;
  while (ok && (n = readChunk(inBuf, 2048)) > 0) {
    sink.updateCrc(inBuf, (size_t)n);
    const uint8_t* inPtr = inBuf;
    size_t inLeft = (size_t)n;
    while (ok && inLeft > 0) {
      size_t inSz = inLeft;
      size_t outSz = 4096;
      tdefl_status st = tdefl_compress(comp, inPtr, &inSz, outBuf, &outSz, TDEFL_NO_FLUSH);
      if (st != TDEFL_STATUS_OKAY) { ok = false; break; }
      if (outSz > 0 && !sink.writeCompressed(outBuf, outSz)) { ok = false; break; }
      inPtr += inSz;
      inLeft -= inSz;
      yield();
    }
  }

  // Flush whatever the compressor is still holding internally.
  if (ok) {
    tdefl_status st;
    do {
      size_t inSz = 0;
      size_t outSz = 4096;
      st = tdefl_compress(comp, nullptr, &inSz, outBuf, &outSz, TDEFL_FINISH);
      if (outSz > 0 && !sink.writeCompressed(outBuf, outSz)) { ok = false; break; }
      yield();
    } while (st == TDEFL_STATUS_OKAY);
    if (ok && st != TDEFL_STATUS_DONE) ok = false;
  }

  heap_caps_free(inBuf);
  heap_caps_free(outBuf);
  heap_caps_free(comp);
  if (!ok) return false;
  return sink.writeTrailer();
}

}  // namespace

bool gzipCompressFileToFile(const String& srcPath, const String& dstPath) {
  File in = SD.open(srcPath, FILE_READ);
  if (!in) {
    Serial.printf("[GZIP] Cannot open source %s\n", srcPath.c_str());
    return false;
  }
  if (SD.exists(dstPath)) SD.remove(dstPath);
  File out = SD.open(dstPath, FILE_WRITE);
  if (!out) {
    Serial.printf("[GZIP] Cannot create %s\n", dstPath.c_str());
    in.close();
    return false;
  }

  GzipSink sink;
  sink.out = out;
  bool ok = compressStream(sink, [&](uint8_t* buf, size_t cap) -> int {
    return in.read(buf, cap);
  });
  in.close();
  out.close();
  if (!ok) SD.remove(dstPath);
  return ok;
}

bool gzipCompressMergedFilesToFile(const std::vector<String>& srcPaths, const String& dstPath) {
  if (srcPaths.empty()) return false;
  if (SD.exists(dstPath)) SD.remove(dstPath);
  File out = SD.open(dstPath, FILE_WRITE);
  if (!out) {
    Serial.printf("[GZIP] Cannot create %s\n", dstPath.c_str());
    return false;
  }

  GzipSink sink;
  sink.out = out;

  size_t nextIdx = 0;
  File cur;
  // Set once the merged output has emitted its first byte (from whichever
  // file successfully opened first) -- every file read after that point
  // gets its 2-line WiGLE header stripped, regardless of which index it is.
  bool headerWritten = false;

  bool ok = compressStream(sink, [&](uint8_t* buf, size_t cap) -> int {
    for (;;) {
      if (cur) {
        int n = cur.read(buf, cap);
        if (n > 0) { headerWritten = true; return n; }
        cur.close();
      }
      if (nextIdx >= srcPaths.size()) return 0;
      cur = SD.open(srcPaths[nextIdx], FILE_READ);
      nextIdx++;
      if (!cur) continue;  // skip a file that fails to open, try the next one
      if (headerWritten) {
        if (cur.available()) cur.readStringUntil('\n');
        if (cur.available()) cur.readStringUntil('\n');
      }
    }
  });

  if (cur) cur.close();
  out.close();
  if (!ok) SD.remove(dstPath);
  return ok;
}
