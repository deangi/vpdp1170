#pragma once
#include <stdint.h>
#include "gfx.h"

// 80x25 ANSI/VT terminal emulator fed by the 8086tiny BIOS PUTCHAR stream.
// Renders to the TFT; keyboard input from any source is queued back to the guest.

#define CON_COLS 80
#define CON_ROWS 25

void console_init();

// Feed one byte of the guest console output stream (BIOS PUTCHAR / ANSI).
// Buffered: KEK is the sole producer and the TFT output task is the sole
// consumer, so neither ANSI parsing nor rendering can block emulation.
void console_feed(uint8_t c);

// Start the dedicated core-0 TFT ANSI-parser consumer. Idempotent.
bool console_start_output_task();

// Output queue diagnostics.
void console_output_stats(uint32_t* pending, uint32_t* dropped);

// Keyboard: bytes typed by the user (serial / telnet / touch), delivered
// to the guest via the BIOS keyboard hook.
void console_key_push(uint8_t c);
int  console_key_pop(uint8_t* out);     // returns 1 if a byte was dequeued

// Draw changed cells to the TFT (call from the main loop).
void console_render(GfxDisplay& tft);
void console_force_redraw();             // mark the whole screen dirty

void console_get_cursor(int* row, int* col);

// Output-activity tracking (used to detect "DOS finished booting" = output
// has gone quiet at a prompt).
uint32_t console_feed_count();           // total bytes fed since boot
uint32_t console_last_feed_ms();         // millis() of the most recent byte
