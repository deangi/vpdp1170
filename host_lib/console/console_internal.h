#pragma once
#include <stdint.h>

// Cell-buffer primitives shared by VT100 and ADM-3A parsers.

void con_put_glyph(uint8_t ch);
void con_bs();
void con_tab();
void con_lf();               // cursor down + scroll; column unchanged
void con_cr();
void con_home();
void con_clear_screen();
void con_cursor_up(int n);
void con_cursor_down(int n);
void con_cursor_left(int n);
void con_cursor_right(int n);
void con_cursor_set(int r, int c);  // 0-based
void con_erase_eol();
int  con_cols();
int  con_rows();
