// ms11.h - DEC MS11 silicon memory module.
//
// Cloned-and-trimmed from sam11's ms11.h. The original conditionally
// declared an `SdFile msdata` for the RAM_SWAPFILE backing; we don't use
// that mode on ESP32-S3 (our ms11.cpp keeps the bytes in PSRAM via the
// `cpu_mem()` pointer from cpu_pdp11.cpp), so the SdFat dependency is
// dropped here.

#include "pdp1140.h"
#include "sam11_platform.h"

#ifndef H_MS11
#define H_MS11

namespace ms11 {
void begin();
void clear();
uint16_t read8(uint32_t a);
void     write8(uint32_t a, uint16_t v);
void     write16(uint32_t a, uint16_t v);
uint16_t read16(uint32_t a);
};

#endif
