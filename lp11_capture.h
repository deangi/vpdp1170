#pragma once

#include <stddef.h>
#include <stdint.h>

// Host-side LP11 print capture: 4 KB SPSC FIFO + SD LPn.TXT consumer task.

namespace lp11_capture {

void init();          // start lp_consumer task once
void begin_session(); // on LP11 reset: flush prior, pick/reuse LPn.TXT

bool push(uint8_t c); // guest producer; updates last-push time
size_t free_space();
size_t count();

const char* current_path();

}  // namespace lp11_capture
