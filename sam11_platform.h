// sam11_platform.h - ESP32-S3 platform shim for vendored sam11 sources.
//
// Replaces sam11's original platform.h (which only knows about AVR Mega /
// SAMD51 / SAMD21 / Teensy 4.1). sam11 sources have been edited to include
// THIS file instead of platform.h, so our own platform.h (LOG/LOGE for
// the host scaffolding) is unaffected.
//
// Memory: sam11's MS11 RAM module normally allocates an in-DRAM
// `int_mem[MAX_RAM_ADDRESS]` array. We do NOT use that path on ESP32-S3
// because (a) DRAM is only ~512 KB and largely spoken for, (b) we want
// the guest RAM in PSRAM. Our replacement `ms11.cpp` in this folder
// reads/writes through the `cpu_mem()` pointer from cpu_pdp11.cpp.
// We still set RAM_MODE = RAM_INTERNAL here for the benefit of the few
// other places that test the macro (none of which matter on ESP32).

#include "pdp1140.h"

#ifndef H_PLATFORM
#define H_PLATFORM

// sam11 sources use Serial / F() / OCT / delay() etc. directly. In the
// upstream they got Arduino.h transitively via <SdFat.h>; we stripped that,
// so pull Arduino.h in here. Every sam11 .cpp/.h includes sam11_platform.h
// so this lands everywhere it's needed.
#include <Arduino.h>

// ESP32-S3's Arduino.h / Xtensa specreg.h / esp_bit_defs.h aggressively
// define short uppercase macros that collide with PDP-11 instruction
// mnemonics and CPU-register identifiers used by sam11. Undef them all
// here so the PDP-11 namespace symbols remain accessible.
//   PS    - Xtensa special register index for Processor Status (clobbers kd11::PS)
//   BR    - Xtensa special register index for Boolean Register (clobbers kd11::BR())
//   BIT   - esp_bit_defs.h BIT(nr) macro (clobbers kd11::BIT())
//   _NOP  - Arduino.h inline-asm helper (clobbers kd11::_NOP())
//   SPL   - sometimes defined; clobbers kd11::SPL() (Set Priority Level)
// Plus a defensive sweep of other PDP-11 mnemonics that could plausibly
// be defined elsewhere.
#ifdef PS
#undef PS
#endif
#ifdef BR
#undef BR
#endif
#ifdef BIT
#undef BIT
#endif
#ifdef _NOP
#undef _NOP
#endif
#ifdef SPL
#undef SPL
#endif
#ifdef BIS
#undef BIS
#endif
#ifdef BIC
#undef BIC
#endif
#ifdef RESET
#undef RESET
#endif
#ifdef _ADC
#undef _ADC
#endif
#ifdef _DEC
#undef _DEC
#endif
#ifdef _HALT
#undef _HALT
#endif
#ifdef _WAIT
#undef _WAIT
#endif

#define RAM_SPI      (0)
#define RAM_INTERNAL (1)
#define RAM_EXTENDED (2)
#define RAM_PARALLEL (3)
#define RAM_SWAPFILE (4)

#define LKS_LOW_ACC    (0)
#define LKS_HIGH_ACC   (2)
#define LKS_SHIFT_TICK (3)

namespace platform {
// sam11's loop calls platform::begin() once; we don't need anything here.
inline void begin() {}
// Front-panel hooks are stubbed; KY_PANEL is false in pdp1140.h.
inline void writeAddr(uint32_t)    {}
inline void writeData(uint16_t)    {}
inline void writeDispReg(uint16_t) {}
inline uint32_t readSwitches()        { return 0; }
inline uint16_t readControlSwitches() { return 0; }
};

// ---- ESP32-S3 settings ----
#define _printf Serial.printf

#define USE_SDIO        false   // we drive disks via our disk.cpp; sam11's rk11 is stubbed

#define ALLOW_DISASM    (false) // saves flash; disasm.cpp body becomes inert

// 18-bit PDP-11/40 address ceiling for normal RAM
// (0..0760000 is RAM, 0760000..0777777 is the I/O page handled by dd11)
#define MAX_RAM_ADDRESS (0760000)

// RAM_INTERNAL is a label - we replace ms11.cpp wholesale with a PSRAM version,
// so the in-DRAM `int_mem[]` is never instantiated.
#define RAM_MODE        RAM_INTERNAL

// LED + pin macros: sam11 only consults these if specific #ifdefs are set;
// we leave them undefined so the conditional code is dropped at compile time.
#define LED_ON  (HIGH)
#define LED_OFF (LOW)

// Line-clock accuracy mode. The HIGH_ACC and LOW_ACC modes pull in
// PaulStoffregen's elapsedMillis library which isn't installed by
// default on ESP32. SHIFT_TICK uses an internal counter, "1 op step
// = 1us" assumption; it's loop-rate dependent so OS clock drifts,
// but it builds out-of-the-box and is good enough for boot.
#define LKS_ACC LKS_SHIFT_TICK

#endif
