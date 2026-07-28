// ms11.cpp - PSRAM-backed MS11 silicon memory for vpdp1140.
//
// Replaces sam11's stock ms11.cpp + ram_opts/ram_int.cpp.h pair, which
// would otherwise allocate `volatile char int_mem[MAX_RAM_ADDRESS]` in
// the ESP32-S3's internal DRAM. We can't afford a 248 KiB DRAM array (it
// would dwarf the 512 KB total and starve WiFi/TFT), so guest RAM lives
// in PSRAM, allocated by cpu_init() in cpu_pdp11.cpp and exposed via the
// `cpu_mem()` accessor.
//
// Public API matches what dd11.cpp and the rest of sam11 expect.

#include "ms11.h"
#include "cpu_pdp11.h"
#include <Arduino.h>

namespace ms11 {

static inline uint8_t* base() { return cpu_mem(); }

void begin() {
    // PSRAM allocation happens in cpu_init() (cpu_pdp11.cpp), which is
    // called BEFORE this. Nothing to do here.
}

void clear() {
    uint8_t* b = base();
    if (b) memset(b, 0, MAX_RAM_ADDRESS);
}

uint16_t read8(uint32_t a) {
    return (uint16_t)base()[a];
}

void write8(uint32_t a, uint16_t v) {
    base()[a] = (uint8_t)(v & 0xFF);
}

uint16_t read16(uint32_t a) {
    // PDP-11 is little-endian word access; dd11 already aligns to even a.
    return ((uint16_t*)base())[a >> 1];
}

void write16(uint32_t a, uint16_t v) {
    ((uint16_t*)base())[a >> 1] = v;
}

};
