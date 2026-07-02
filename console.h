#pragma once
#include <stdint.h>

// 80x25 ANSI/VT terminal emulator fed by the 8086tiny BIOS PUTCHAR stream.
// Renders to the TFT; keyboard input from any source is queued back to the guest.

#define CON_COLS 80
#define CON_ROWS 25

class TFT_eSPI;

void console_init();

// Feed one byte of the guest console output stream (BIOS PUTCHAR / ANSI).
// Buffered: the byte is queued into an 8 KB FIFO and rendered by
// console_drain_tft() on the next loop slice, so kl11::poll() never
// blocks on the cell grid.
void console_feed(uint8_t c);

// Drain queued bytes through the ANSI parser into the cell grid. Call
// from loop() on core 1. render_task (core 0) reads the cell grid.
void console_drain_tft();

// Keyboard: bytes typed by the user (serial / telnet / touch), delivered
// to the guest via the BIOS keyboard hook.
void console_key_push(uint8_t c);
int  console_key_pop(uint8_t* out);     // returns 1 if a byte was dequeued

// Draw changed cells to the TFT (call from the main loop).
void console_render(TFT_eSPI& tft);
void console_force_redraw();             // mark the whole screen dirty

void console_get_cursor(int* row, int* col);

// Output-activity tracking (used to detect "DOS finished booting" = output
// has gone quiet at a prompt).
uint32_t console_feed_count();           // total bytes fed since boot
uint32_t console_last_feed_ms();         // millis() of the most recent byte
