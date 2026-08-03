#pragma once
#include <stdint.h>

// Host diagnostic surface for KW11-L clock tracing. Guest device is kek.
namespace kw11 {

void set_clock_trace(uint32_t count);
uint32_t clock_trace_remaining();
// what: "tick", "irq-sched", "guest-rd", "guest-wr", etc.
void charge_clock_trace(const char* what, uint16_t csr, uint16_t extra = 0);

}  // namespace kw11
