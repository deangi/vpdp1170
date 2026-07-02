// DEC KW11-P programmable real-time clock at 0o772540.
// Implements the CSR, count-set buffer, live counter, four rates,
// up/down counting, manual tick, one-shot/repeat, DONE/ERR, and the
// vector 0104 BR6 interrupt.

#pragma once
#include "pdp1140.h"

namespace kwp {

extern uint16_t CSR;
extern uint16_t CSB;
extern uint16_t CNTR;

// Runtime gate. When false, the compatibility window reads as zero and
// discards writes. Set from pdpconfig.ini before CPU reset.
extern bool enabled;

void     reset();
void     tick();
uint16_t read16(uint32_t a);
void     write16(uint32_t a, uint16_t v);

};  // namespace kwp
