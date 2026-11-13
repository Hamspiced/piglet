#pragma once
#include <ArduinoJson.h>
#include "esp_heap_caps.h"

// Routes ArduinoJson's JsonDocument allocations to PSRAM (MALLOC_CAP_SPIRAM)
// instead of the library's default malloc()-backed internal heap. On this
// chip, plain malloc() (and therefore every un-annotated JsonDocument used
// across the Web UI's status/files/mesh/update JSON handlers) pulls from the
// same general internal RAM pool that MALLOC_CAP_DMA is carved from -- the
// narrow region the WiFi/crypto hardware needs for TLS handshakes. Every one
// of those JSON buffers competed with and fragmented that pool, even though
// none of them actually need DMA capability. Moving them to PSRAM frees that
// headroom for TLS instead.
class PsramJsonAllocator : public ArduinoJson::Allocator {
 public:
  void* allocate(size_t size) override {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  }
  void deallocate(void* ptr) override {
    heap_caps_free(ptr);
  }
  void* reallocate(void* ptr, size_t new_size) override {
    return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM);
  }
};

// Single shared instance -- inline function-local statics are guaranteed to
// be the same object across every translation unit that calls this.
inline ArduinoJson::Allocator* psramJsonAllocator() {
  static PsramJsonAllocator instance;
  return &instance;
}
