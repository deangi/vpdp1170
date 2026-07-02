// sam11.h - minimal replacement for sam11's own sam11.h.
//
// Sam11's original sam11.h pulls in SdFat, declares the global SdFat `sd`
// instance, and exposes the simulator's debug flags + helper functions
// (printstate / panic / disasm / trap). We don't include sam11.cpp (our
// vpdp1140.ino owns setup()/loop()), so we provide a smaller header that
// just satisfies the cross-module references from kd11.cpp / dd11.cpp /
// kt11.cpp / kl11.cpp / ky11.cpp / lp11.cpp / disasm.cpp / fp11.cpp /
// kb11.cpp.

#include "pdp1140.h"
#include <stddef.h>

#ifndef H_SAM11
#define H_SAM11

// Simulator debug flags (referenced as enum constants by sam11 sources).
// All disabled at compile time; flip selectively when debugging.
enum
{
    PRINTINSTR    = false,
    PRINTSTATE    = false,
    DEBUG_INTER   = false,
    DEBUG_TRAP    = false,
    DEBUG_RK05    = false,
    DEBUG_MMU     = false,
    BREAK_ON_TRAP = false,
    VTRAP_ON_NL   = false,
    PRINTSIMLINES = false,
};

// User-mode string tables (referenced by debug prints in kt11.cpp etc.)
extern const char* users_str[];
extern const char  users_char[];

// Helper functions sam11 sources call. Definitions live in cpu_pdp11.cpp.
void printstate();
void panic();
void disasm(uint32_t ia);
bool disasm_format(uint32_t ia, uint16_t virtual_address,
                   char* buffer, size_t size);
void trap(uint16_t num);

#endif
