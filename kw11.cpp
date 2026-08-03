// Host-side KW11-L clock-trace for the kek path.
// Guest KW11-L state lives in kek kw11-l; ticks are driven by
// pdp_core_kek::service_line_clock().
#include "kw11.h"

#include "platform.h"

namespace kw11 {

static uint32_t clock_trace_count = 0;

void set_clock_trace(uint32_t count) {
  clock_trace_count = count;
}

uint32_t clock_trace_remaining() {
  return clock_trace_count;
}

void charge_clock_trace(const char* what, uint16_t csr, uint16_t extra) {
  if (clock_trace_count == 0) return;
  clock_trace_count--;
  LOG("CLOCK %s LKS=%06o IE=%u DONE=%u extra=%06o remaining=%u",
      what ? what : "?",
      (unsigned)csr,
      (csr & 0100) ? 1 : 0,
      (csr & 0200) ? 1 : 0,
      (unsigned)extra,
      (unsigned)clock_trace_count);
}

}  // namespace kw11
