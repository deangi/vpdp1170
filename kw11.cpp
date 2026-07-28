// Host-side KW11-L clock-trace stubs. Guest KW11-L is kek kw11-l via
// kek_src_kw11_l.cpp.
#include "kw11.h"

namespace kw11 {

static uint32_t clock_trace_count = 0;

void set_clock_trace(uint32_t count) {
  clock_trace_count = count;
}

uint32_t clock_trace_remaining() {
  return clock_trace_count;
}

}  // namespace kw11
