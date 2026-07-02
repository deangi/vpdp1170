#pragma once
#include <stdint.h>

class TFT_eSPI;

void ui_init();

// Open the settings menu (caller detects the double-tap gesture).
void ui_open();

// True while the settings menu is open (the caller should pause the CPU).
bool ui_is_open();

// Feed a tap to the open menu. Returns true if the UI consumed it.
bool ui_handle_tap(int x, int y);

// Draw the menu overlay when open and the screen needs a repaint.
void ui_draw(TFT_eSPI& tft);

// One-shot: returns true once if the user asked to reboot the PDP-11.
bool ui_consume_reboot();

// One-shot: returns true once if the user changed boot media/controller and
// the host should remount disks and reboot the PDP-11.
bool ui_consume_boot_change();

// One-shot: returns true once if the user asked to fully reset the ESP32.
// loop() consumes this and calls ESP.restart() outside the tap handler,
// so the UI mutex is released and the host-side Serial drain has a chance
// to flush before the chip resets.
bool ui_consume_esp_restart();
