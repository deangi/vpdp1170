#include "touch.h"
#include "config.h"
#include "platform.h"
#include "FT6336U.h"

static FT6336U ft(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT);
static bool    was_down = false;

void touch_init() {
  ft.begin();
  LOG("touch: FT6336U firmware id 0x%02X", ft.read_firmware_id());
}

bool touch_poll(int* x, int* y) {
  FT6336U_TouchPointType tp = ft.scan();
  bool down = (tp.touch_count != 0);
  bool tap  = false;

  if (down && !was_down) {
    // Landscape (rotation 1) mapping - same as the Freenove touch tutorial.
    // This FT6336U reports Y about 22 px below the visible pixel position on
    // this display, so compensate here before UI hit testing.
    int sx = tp.tp[0].y;
    int sy = 240 - tp.tp[0].x - 22;
    if (sx < 0) sx = 0; else if (sx > 319) sx = 319;
    if (sy < 0) sy = 0; else if (sy > 239) sy = 239;
    if (x) *x = sx;
    if (y) *y = sy;
    tap = true;
  }
  was_down = down;
  return tap;
}
