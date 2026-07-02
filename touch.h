#pragma once
#include <stdint.h>

// Capacitive touch (FT6336U). Coordinates are mapped to the landscape
// (rotation 1) TFT space: x 0..319, y 0..239.

void touch_init();

// Poll the panel. Returns true exactly once per new touch-down ("tap"),
// writing the tap location into *x,*y. Call once per main-loop iteration.
bool touch_poll(int* x, int* y);
