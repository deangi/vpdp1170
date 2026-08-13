#pragma once

#include <stddef.h>
#include <stdint.h>

// Host-side line-printer capture: 4 KB SPSC FIFO + SD LPn.TXT consumer task.
// Guest adapters (e.g. kek LP11) push bytes only.

namespace lp_capture {

void init();
void begin_session();

bool push(uint8_t c);
size_t free_space();
size_t count();

const char* current_path();

}  // namespace lp_capture
