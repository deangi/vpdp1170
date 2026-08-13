#pragma once
#ifndef HOST_LIB_CONSOLE_H
#define HOST_LIB_CONSOLE_H
#include <stddef.h>
#include <stdint.h>
#include "gfx.h"
#include "term_personality.h"

// 80x25 cell console + key FIFO. Default parser is VT100/CSI (vpdp / v8088).
// Call console_set_personality(HOST_TERM_ADM3A) for vZ80 — never VT100 there.

#define CON_COLS 80
#define CON_ROWS 25

void console_init();
void console_set_personality(HostTermPersonality p);
HostTermPersonality console_personality();

// Feed one byte of the guest console output stream (BIOS PUTCHAR / ANSI).
// Buffered: KEK is the sole producer and the TFT output task is the sole
// consumer, so neither ANSI parsing nor rendering can block emulation.
void console_feed(uint8_t c);

// Start the dedicated core-0 TFT ANSI-parser consumer. Idempotent.
bool console_start_output_task();

// Output queue diagnostics.
void console_output_stats(uint32_t* pending, uint32_t* dropped);
bool console_output_has_space(size_t bytes);

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

#endif  // HOST_LIB_CONSOLE_H
