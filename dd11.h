#pragma once
#include <stdint.h>

// Host diagnostic surface only. Guest UNIBUS dispatch is kek bus.cpp.
namespace dd11 {

void set_io_trace(uint32_t count);
uint32_t io_trace_remaining();

}  // namespace dd11
