#pragma once
#include <stdint.h>

// Host diagnostic surface only. Guest UNIBUS dispatch is kek bus.cpp.
namespace dd11 {

// When true, intended to absorb KE11-A / TT1 probes. Currently honored only
// as a config flag on the kek path (kek bus does not yet apply the absorbs).
extern bool v4b_quirks_enabled;

void set_io_trace(uint32_t count);
uint32_t io_trace_remaining();

}  // namespace dd11
