// Host-side diagnostic stubs formerly implemented by the sam11 DD11 bus.
// Guest I/O dispatch lives in kek bus.cpp; these APIs remain so pdpconfig
// and the telnet shell can set/query the same names.
#include "dd11.h"

namespace dd11 {

static uint32_t io_trace_count = 0;

void set_io_trace(uint32_t count) {
  io_trace_count = count;
}

uint32_t io_trace_remaining() {
  return io_trace_count;
}

}  // namespace dd11
