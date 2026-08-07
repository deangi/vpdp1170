#pragma once
#include <stdint.h>
#include "gfx.h"

// Capacitive touch. Coordinates are in display draw space:
//   Freenove 2.8":  x 0..319, y 0..239 (FT6336U, landscape mapped)
//   CrowPanel 7":   x 0..799, y 0..479 (GT911 via LovyanGFX getTouch)

// Pass &tft so CrowPanel can call LGFX::getTouch(). Freenove ignores it.
void touch_init(GfxDisplay* display = nullptr);

// Poll the panel. Returns true once per confirmed contact (after a short
// press debounce), writing the tap location into *x,*y. Call once per
// main-loop / render-task iteration.
bool touch_poll(int* x, int* y);
