#include "touch.h"
#include "config.h"
#include "platform.h"

#include <Arduino.h>

// Capacitive controllers occasionally report 1–2 sample contact glitches.
// A rising edge that lasts < TOUCH_PRESS_MS is ignored; a brief release
// during a real press (< TOUCH_RELEASE_MS) does not start a new tap.
// Without this, two noise edges inside UI_DOUBLE_TAP_MS open the settings
// menu as if the user double-tapped.
static constexpr uint32_t TOUCH_PRESS_MS   = 40;
static constexpr uint32_t TOUCH_RELEASE_MS = 50;

#if VPDP_TOUCH_BACKEND == VPDP_TOUCH_FT6336U
// ---- Freenove 2.8": FT6336U over I2C ----
#include "FT6336U.h"

static FT6336U ft(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT);
static bool     was_down     = false;
static bool     tap_emitted  = false;
static uint32_t down_since_ms = 0;
static uint32_t up_since_ms   = 0;
static int      pending_x = 0;
static int      pending_y = 0;

void touch_init(GfxDisplay* display) {
  (void)display;
  ft.begin();
}

bool touch_poll(int* x, int* y) {
  FT6336U_TouchPointType tp = ft.scan();
  const bool raw_down = (tp.touch_count != 0);
  const uint32_t now = millis();
  bool tap = false;

  if (raw_down) {
    up_since_ms = 0;
    // Landscape (rotation 1) mapping - same as the Freenove touch tutorial.
    // This FT6336U reports Y about 22 px below the visible pixel position on
    // this display, so compensate here before UI hit testing.
    int sx = tp.tp[0].y;
    int sy = 240 - tp.tp[0].x - 22;
    if (sx < 0) sx = 0; else if (sx > 319) sx = 319;
    if (sy < 0) sy = 0; else if (sy > 239) sy = 239;

    if (!was_down) {
      was_down = true;
      tap_emitted = false;
      down_since_ms = now;
      pending_x = sx;
      pending_y = sy;
    } else {
      pending_x = sx;
      pending_y = sy;
      if (!tap_emitted &&
          (uint32_t)(now - down_since_ms) >= TOUCH_PRESS_MS) {
        if (x) *x = pending_x;
        if (y) *y = pending_y;
        tap = true;
        tap_emitted = true;
      }
    }
  } else if (was_down) {
    if (up_since_ms == 0)
      up_since_ms = now;
    if ((uint32_t)(now - up_since_ms) >= TOUCH_RELEASE_MS) {
      was_down = false;
      tap_emitted = false;
      up_since_ms = 0;
    }
  }
  return tap;
}

#elif VPDP_TOUCH_BACKEND == VPDP_TOUCH_GT911
// ---- CrowPanel 7": GT911 owned by LovyanGFX after gfx.init ----
// Do not reclaim Wire / talk to STC8H after init — that tears down GT911 I2C.
//
// Orientation: the panel is drawn 180° via cfg.offset_rotation=2 in
// lgfx_conf.h, but we deliberately never call setRotation() on this board, so
// LGFX::getTouch() reports coordinates in the *runtime* rotation (0) — i.e.
// raw, un-rotated space. That leaves touch 180° off from what is on screen
// (a tap top-left registers bottom-right). We apply the matching 180° flip
// here so tap coordinates land in the same draw space as the UI.
//   VPDP_TOUCH_FLIP_180 = 1  → sx = (W-1)-tx,  sy = (H-1)-ty
// If a future panel/library revision starts honoring offset_rotation in
// getTouch(), set this to 0.
#ifndef VPDP_TOUCH_FLIP_180
#define VPDP_TOUCH_FLIP_180 1
#endif

static GfxDisplay* g_gfx = nullptr;
static bool     was_down     = false;
static bool     tap_emitted  = false;
static uint32_t down_since_ms = 0;
static uint32_t up_since_ms   = 0;
static int      pending_x = 0;
static int      pending_y = 0;

void touch_init(GfxDisplay* display) {
  g_gfx = display;
  if (!g_gfx) {
    LOGE("touch: GT911 needs display pointer (touch_init(&tft))");
    return;
  }
  LOG("touch: GT911 via LovyanGFX getTouch()  %dx%d  flip180=%d",
      TFT_W, TFT_H, VPDP_TOUCH_FLIP_180);
}

bool touch_poll(int* x, int* y) {
  if (!g_gfx) return false;

  uint16_t tx = 0, ty = 0;
  const bool raw_down = g_gfx->getTouch(&tx, &ty);
  const uint32_t now = millis();
  bool tap = false;

  if (raw_down) {
    up_since_ms = 0;
    int sx = (int)tx;
    int sy = (int)ty;
#if VPDP_TOUCH_FLIP_180
    sx = (TFT_W - 1) - sx;
    sy = (TFT_H - 1) - sy;
#endif
    if (sx < 0) sx = 0; else if (sx >= TFT_W) sx = TFT_W - 1;
    if (sy < 0) sy = 0; else if (sy >= TFT_H) sy = TFT_H - 1;

    if (!was_down) {
      was_down = true;
      tap_emitted = false;
      down_since_ms = now;
      pending_x = sx;
      pending_y = sy;
    } else {
      pending_x = sx;
      pending_y = sy;
      if (!tap_emitted &&
          (uint32_t)(now - down_since_ms) >= TOUCH_PRESS_MS) {
        if (x) *x = pending_x;
        if (y) *y = pending_y;
        tap = true;
        tap_emitted = true;
      }
    }
  } else if (was_down) {
    if (up_since_ms == 0)
      up_since_ms = now;
    if ((uint32_t)(now - up_since_ms) >= TOUCH_RELEASE_MS) {
      was_down = false;
      tap_emitted = false;
      up_since_ms = 0;
    }
  }
  return tap;
}

#else
#error "Unknown VPDP_TOUCH_BACKEND"
#endif
