#include "console.h"
#include "config.h"
#include "platform.h"
#include "font4x8.h"
#include "fifo.h"
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <string.h>
#include "esp_attr.h"
#ifndef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#endif

// ---- 16-colour CGA palette in RGB565 ----
static const uint16_t kPalette[16] = {
  0x0000, // 0 black
  0x0015, // 1 blue
  0x0540, // 2 green
  0x0555, // 3 cyan
  0xA800, // 4 red
  0xA815, // 5 magenta
  0xAAA0, // 6 brown
  0xAD55, // 7 light grey
  0x52AA, // 8 dark grey
  0x001F, // 9 bright blue
  0x07E0, // 10 bright green
  0x07FF, // 11 bright cyan
  0xF800, // 12 bright red
  0xF81F, // 13 bright magenta
  0xFFE0, // 14 yellow
  0xFFFF, // 15 white
};

// ANSI SGR fg/bg code (30-37) -> CGA colour index
static const uint8_t kAnsiToCga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

#define DEF_ATTR     0x07      // light grey on black
#define ATTR_INVERSE 0x80

// ---- screen state ----
static uint8_t  cell_ch[CON_ROWS][CON_COLS];
static uint8_t  cell_at[CON_ROWS][CON_COLS];
static uint8_t  shad_ch[CON_ROWS][CON_COLS];
static uint8_t  shad_at[CON_ROWS][CON_COLS];
static bool     shad_valid = false;

static int  cur_r = 0, cur_c = 0;
static int  prev_cur_r = 0, prev_cur_c = 0;
static uint8_t cur_attr = DEF_ATTR;
static int  sr_top = 0, sr_bot = CON_ROWS - 1;
static int  saved_r = 0, saved_c = 0;

// ---- ANSI parser ----
static enum { ST_GROUND, ST_ESC, ST_CSI, ST_CHARSET_DESIGNATE } ansi_st = ST_GROUND;
static int  csi_param[8];
static int  csi_nparam = 0;
static bool csi_has_digit = false;
static bool csi_private = false;

typedef enum { CHARSET_ASCII, CHARSET_VT100_GRAPHICS } ConsoleCharset;

static ConsoleCharset g0_charset = CHARSET_ASCII;
static enum { ACTIVE_G0, ACTIVE_G1 } active_charset = ACTIVE_G0;
static ConsoleCharset g1_charset = CHARSET_ASCII;
static uint8_t charset_designate_target = 0;

#define GLYPH_HLINE 0x80
#define GLYPH_VLINE 0x81
#define GLYPH_UL    0x82
#define GLYPH_UR    0x83
#define GLYPH_LL    0x84
#define GLYPH_LR    0x85
#define GLYPH_LTEE  0x86
#define GLYPH_RTEE  0x87
#define GLYPH_BTEE  0x88
#define GLYPH_TTEE  0x89
#define GLYPH_CROSS 0x8A

// ---- 8 KB serial-input FIFO (host USB-Serial -> KL11 TKB) and 8 KB
//      TFT-output FIFO (KL11 TPB -> ANSI parser -> cell grid). Both live
//      in PSRAM via EXT_RAM_BSS_ATTR; the indices stay in DRAM. Producer
//      and consumer for both are on core 1 (loop()), so plain volatile
//      head/tail is sufficient. ----
#define VPDP_FIFO_BYTES 8192   // must be power of two
EXT_RAM_BSS_ATTR static uint8_t serial_in_storage[VPDP_FIFO_BYTES];
EXT_RAM_BSS_ATTR static uint8_t tft_out_storage[VPDP_FIFO_BYTES];
static Fifo g_serial_in;
static Fifo g_tft_out;

// ---- output activity ----
static uint32_t g_feed_count   = 0;
static uint32_t g_last_feed_ms = 0;

// -------------------------------------------------------------------------

static void clear_row(int r, uint8_t attr) {
  for (int c = 0; c < CON_COLS; c++) { cell_ch[r][c] = ' '; cell_at[r][c] = attr; }
}

void console_init() {
  for (int r = 0; r < CON_ROWS; r++) clear_row(r, DEF_ATTR);
  cur_r = cur_c = 0;
  prev_cur_r = prev_cur_c = 0;
  cur_attr = DEF_ATTR;
  sr_top = 0; sr_bot = CON_ROWS - 1;
  ansi_st = ST_GROUND;
  csi_private = false;
  g0_charset = CHARSET_ASCII;
  g1_charset = CHARSET_ASCII;
  active_charset = ACTIVE_G0;
  shad_valid = false;
  g_serial_in.init(serial_in_storage, VPDP_FIFO_BYTES);
  g_tft_out.init(tft_out_storage, VPDP_FIFO_BYTES);
}

void console_force_redraw() { shad_valid = false; }

void console_get_cursor(int* row, int* col) {
  if (row) *row = cur_r;
  if (col) *col = cur_c;
}

// ---- scrolling within the current scroll region ----
static void scroll_region_up() {
  for (int r = sr_top; r < sr_bot; r++) {
    memcpy(cell_ch[r], cell_ch[r + 1], CON_COLS);
    memcpy(cell_at[r], cell_at[r + 1], CON_COLS);
  }
  clear_row(sr_bot, cur_attr);
}

static void scroll_region_down() {
  for (int r = sr_bot; r > sr_top; r--) {
    memcpy(cell_ch[r], cell_ch[r - 1], CON_COLS);
    memcpy(cell_at[r], cell_at[r - 1], CON_COLS);
  }
  clear_row(sr_top, cur_attr);
}

static void cursor_down_scroll() {
  if (cur_r >= sr_bot) {
    scroll_region_up();
    cur_r = sr_bot;
  } else if (cur_r < CON_ROWS - 1) {
    cur_r++;
  }
}

static void cursor_up_scroll() {
  if (cur_r <= sr_top) {
    scroll_region_down();
    cur_r = sr_top;
  } else if (cur_r > 0) {
    cur_r--;
  }
}

static void clamp_cursor() {
  if (cur_c < 0) cur_c = 0;
  if (cur_c >= CON_COLS) cur_c = CON_COLS - 1;
  if (cur_r < 0) cur_r = 0;
  if (cur_r >= CON_ROWS) cur_r = CON_ROWS - 1;
}

// ---- printable character ----
static uint8_t translate_printable(uint8_t ch) {
  uint8_t charset = (active_charset == ACTIVE_G1) ? g1_charset : g0_charset;
  if (charset != CHARSET_VT100_GRAPHICS) return ch;

  switch (ch) {
    case 'q': return GLYPH_HLINE;
    case 'x': return GLYPH_VLINE;
    case 'l': return GLYPH_UL;
    case 'k': return GLYPH_UR;
    case 'm': return GLYPH_LL;
    case 'j': return GLYPH_LR;
    case 't': return GLYPH_LTEE;
    case 'u': return GLYPH_RTEE;
    case 'v': return GLYPH_BTEE;
    case 'w': return GLYPH_TTEE;
    case 'n': return GLYPH_CROSS;
    default:  return ch;
  }
}

static void put_glyph(uint8_t ch) {
  if (cur_c >= CON_COLS) { cur_c = 0; cursor_down_scroll(); }
  cell_ch[cur_r][cur_c] = translate_printable(ch);
  cell_at[cur_r][cur_c] = cur_attr;
  cur_c++;
}

// ---- SGR (colour) ----
static void apply_sgr() {
  if (csi_nparam == 0) { cur_attr = DEF_ATTR; return; }
  for (int i = 0; i < csi_nparam; i++) {
    int p = csi_param[i];
    if (p == 0)                       cur_attr = DEF_ATTR;
    else if (p == 1)                  cur_attr |= 0x08;            // bright fg
    else if (p == 7)                  cur_attr |= ATTR_INVERSE;    // inverse video
    else if (p == 27)                 cur_attr &= ~ATTR_INVERSE;   // inverse off
    else if (p >= 30 && p <= 37)      cur_attr = (cur_attr & 0xF8) | kAnsiToCga[p - 30];
    else if (p >= 40 && p <= 47)      cur_attr = (cur_attr & 0x8F) | (kAnsiToCga[p - 40] << 4);
    else if (p >= 90 && p <= 97)      cur_attr = (cur_attr & 0xF0) | kAnsiToCga[p - 90] | 0x08;
  }
}

static void exec_private_csi(uint8_t final) {
  if (final != 'h' && final != 'l') return;

  // DEC private modes. These affect local keyboard/display behavior on a real
  // VT100; the TFT console only needs to consume them cleanly.
  for (int i = 0; i < csi_nparam; i++) {
    switch (csi_param[i]) {
      case 1:   // DECCKM - cursor key mode
      case 7:   // DECAWM - autowrap
      case 8:   // DECARM - keyboard autorepeat
      case 25:  // DECTCEM - cursor visibility
        break;
      default:
        break;
    }
  }
}

// ---- erase ----
static void erase_in_display(int mode) {
  // Compatibility: some PDP software treats clear-screen as a display reset
  // and only sends SI/ESC[m before repainting text. Real VT100 charset
  // designations survive ED, but selecting G0 here prevents stale SO state
  // from leaking line-drawing characters onto the next screen.
  active_charset = ACTIVE_G0;

  if (mode == 2 || mode == 3) {
    for (int r = 0; r < CON_ROWS; r++) clear_row(r, cur_attr);
  } else if (mode == 0) {                       // cursor -> end
    for (int c = cur_c; c < CON_COLS; c++) { cell_ch[cur_r][c] = ' '; cell_at[cur_r][c] = cur_attr; }
    for (int r = cur_r + 1; r < CON_ROWS; r++) clear_row(r, cur_attr);
  } else if (mode == 1) {                       // start -> cursor
    for (int r = 0; r < cur_r; r++) clear_row(r, cur_attr);
    for (int c = 0; c <= cur_c && c < CON_COLS; c++) { cell_ch[cur_r][c] = ' '; cell_at[cur_r][c] = cur_attr; }
  }
}

static void erase_in_line(int mode) {
  if (mode == 0)      for (int c = cur_c; c < CON_COLS; c++) { cell_ch[cur_r][c] = ' '; cell_at[cur_r][c] = cur_attr; }
  else if (mode == 1) for (int c = 0; c <= cur_c && c < CON_COLS; c++) { cell_ch[cur_r][c] = ' '; cell_at[cur_r][c] = cur_attr; }
  else                clear_row(cur_r, cur_attr);
}

// ---- execute a completed CSI sequence ----
static void exec_csi(uint8_t final) {
  if (csi_private) {
    exec_private_csi(final);
    return;
  }

  int p0 = csi_nparam > 0 ? csi_param[0] : 0;
  int p1 = csi_nparam > 1 ? csi_param[1] : 0;
  switch (final) {
    case 'H': case 'f':                          // cursor position (1-based)
      cur_r = (csi_nparam > 0 ? p0 : 1) - 1;
      cur_c = (csi_nparam > 1 ? p1 : 1) - 1;
      clamp_cursor();
      break;
    case 'A': cur_r -= (p0 ? p0 : 1); clamp_cursor(); break;
    case 'B': cur_r += (p0 ? p0 : 1); clamp_cursor(); break;
    case 'C': cur_c += (p0 ? p0 : 1); clamp_cursor(); break;
    case 'D': cur_c -= (p0 ? p0 : 1); clamp_cursor(); break;
    case 'J': erase_in_display(p0); break;
    case 'K': erase_in_line(p0); break;
    case 'm': apply_sgr(); break;
    case 'r':                                    // scroll region
      sr_top = (csi_nparam > 0 ? p0 : 1) - 1;
      sr_bot = (csi_nparam > 1 ? p1 : CON_ROWS) - 1;
      if (sr_top < 0) sr_top = 0;
      if (sr_bot >= CON_ROWS) sr_bot = CON_ROWS - 1;
      if (sr_top > sr_bot) { sr_top = 0; sr_bot = CON_ROWS - 1; }
      cur_r = 0;
      cur_c = 0;
      break;
    case 's': saved_r = cur_r; saved_c = cur_c; break;
    case 'u': cur_r = saved_r; cur_c = saved_c; clamp_cursor(); break;
    case 'M':                                    // scroll up (1 line)
      for (int i = 0; i < (p0 ? p0 : 1); i++) scroll_region_up();
      break;
    case 'S':                                    // scroll up N
      for (int i = 0; i < (p0 ? p0 : 1); i++) scroll_region_up();
      break;
    case 'T':                                    // scroll down N
      for (int i = 0; i < (p0 ? p0 : 1); i++) scroll_region_down();
      break;
    case 'L':                                    // insert lines at cursor
      for (int i = 0; i < (p0 ? p0 : 1); i++) {
        for (int r = sr_bot; r > cur_r; r--) {
          memcpy(cell_ch[r], cell_ch[r - 1], CON_COLS);
          memcpy(cell_at[r], cell_at[r - 1], CON_COLS);
        }
        clear_row(cur_r, cur_attr);
      }
      break;
    default: break;
  }
}

// -------------------------------------------------------------------------
// Internal ANSI-parser entrypoint. Runs on core 1 from console_drain_tft().
// Updates cell_ch/cell_at, which render_task on core 0 reads without a
// lock (single-byte cells tolerate the race).
static void feed_ansi(uint8_t c) {
  g_feed_count++;
  g_last_feed_ms = millis();
  switch (ansi_st) {
    case ST_GROUND:
      switch (c) {
        case 0x1B: ansi_st = ST_ESC; break;
        case 0x0E: active_charset = ACTIVE_G1; break;              // SO
        case 0x0F: active_charset = ACTIVE_G0; break;              // SI
        case 0x07: break;                                  // BEL - ignore
        case 0x08: if (cur_c > 0) cur_c--; break;           // BS
        case 0x09:                                          // TAB
          cur_c = (cur_c + 8) & ~7;
          if (cur_c >= CON_COLS) cur_c = CON_COLS - 1;
          break;
        case 0x0A: cur_c = 0; cursor_down_scroll(); break;  // LF (BIOS: CR+LF)
        case 0x0D: cur_c = 0; break;                        // CR
        default:
          if (c >= 0x20) put_glyph(c);
          break;
      }
      break;

    case ST_ESC:
      if (c == '[') {
        ansi_st = ST_CSI;
        csi_nparam = 0; csi_param[0] = 0; csi_has_digit = false; csi_private = false;
      } else if (c == '(' || c == ')') {
        charset_designate_target = (c == '(') ? 0 : 1;
        ansi_st = ST_CHARSET_DESIGNATE;
      } else if (c == 'D') {                                      // IND
        cursor_down_scroll();
        ansi_st = ST_GROUND;
      } else if (c == 'E') {                                      // NEL
        cur_c = 0;
        cursor_down_scroll();
        ansi_st = ST_GROUND;
      } else if (c == 'M') {                                      // RI
        cursor_up_scroll();
        ansi_st = ST_GROUND;
      } else if (c == '7') {
        saved_r = cur_r; saved_c = cur_c;
        ansi_st = ST_GROUND;
      } else if (c == '8') {
        cur_r = saved_r; cur_c = saved_c; clamp_cursor();
        ansi_st = ST_GROUND;
      } else {
        ansi_st = ST_GROUND;       // other ESC sequences not supported
      }
      break;

    case ST_CSI:
      if (c >= '0' && c <= '9') {
        if (csi_nparam == 0) csi_nparam = 1;
        csi_param[csi_nparam - 1] = csi_param[csi_nparam - 1] * 10 + (c - '0');
        csi_has_digit = true;
      } else if (c == ';') {
        if (csi_nparam == 0) csi_nparam = 1;
        if (csi_nparam < 8) { csi_param[csi_nparam] = 0; csi_nparam++; }
        csi_has_digit = false;
      } else if (c == '?' && csi_nparam == 0 && !csi_has_digit) {
        csi_private = true;
      } else if (c == '>' || c == '=') {
        // Other private-mode introducers - consume and keep parsing.
      } else if (c >= 0x40 && c <= 0x7E) {
        exec_csi(c);
        ansi_st = ST_GROUND;
        csi_private = false;
      } else {
        ansi_st = ST_GROUND;       // malformed - bail
        csi_private = false;
      }
      break;

    case ST_CHARSET_DESIGNATE: {
      ConsoleCharset charset = (c == '0') ? CHARSET_VT100_GRAPHICS : CHARSET_ASCII;
      if (charset_designate_target == 0) {
        g0_charset = charset;
      } else {
        g1_charset = charset;
        // Some hosts send ESC ) 0 / ESC ) B without explicit SO/SI.
        active_charset = (charset == CHARSET_VT100_GRAPHICS) ? ACTIVE_G1 : ACTIVE_G0;
      }
      ansi_st = ST_GROUND;
      break;
    }
  }
}

#ifdef CONSOLE_VT100_SELFTEST
static bool console_vt100_selftest() {
  console_init();
  const uint8_t decarm[] = { 0x1B, '[', '?', '8', 'l', 0x1B, '[', '?', '8', 'h', 'A' };
  for (uint8_t b : decarm) feed_ansi(b);
  if (cell_ch[0][0] != 'A') return false;

  console_init();
  const uint8_t inverse[] = { 0x1B, '[', '7', 'm', 'B', 0x1B, '[', 'm', 'C' };
  for (uint8_t b : inverse) feed_ansi(b);
  if ((cell_at[0][0] & ATTR_INVERSE) == 0) return false;
  if ((cell_at[0][1] & ATTR_INVERSE) != 0) return false;

  console_init();
  const uint8_t g0_box[] = { 0x1B, '(', '0', 'l', 'q', 'k', 0x1B, '(', 'B' };
  for (uint8_t b : g0_box) feed_ansi(b);
  if (cell_ch[0][0] != GLYPH_UL || cell_ch[0][1] != GLYPH_HLINE || cell_ch[0][2] != GLYPH_UR) return false;

  console_init();
  const uint8_t g1_compat[] = { 0x1B, ')', '0', 'q', 'q', 'q', 0x1B, ')', 'B', 'q' };
  for (uint8_t b : g1_compat) feed_ansi(b);
  if (cell_ch[0][0] != GLYPH_HLINE || cell_ch[0][1] != GLYPH_HLINE || cell_ch[0][2] != GLYPH_HLINE) return false;
  if (cell_ch[0][3] != 'q') return false;

  return true;
}
#endif

// ---- TFT-out FIFO: KL11 push, main-loop drain ----
// Public entrypoint kl11::poll() calls per output byte. Just buffers.
void console_feed(uint8_t c) {
  g_tft_out.push(c);          // drop-newest if full (sink falls behind)
}

// Drain pending TFT bytes through the ANSI parser. Called from loop() on
// core 1 once per slice so the cell grid updates in roughly real time.
void console_drain_tft() {
  uint8_t b;
  while (g_tft_out.pop(&b)) feed_ansi(b);
}

// ---- keyboard (host USB-Serial -> KL11 input FIFO) ----
void console_key_push(uint8_t c) { g_serial_in.push(c); }
int  console_key_pop(uint8_t* out) { return g_serial_in.pop(out) ? 1 : 0; }

uint32_t console_feed_count()   { return g_feed_count; }
uint32_t console_last_feed_ms() { return g_last_feed_ms; }

// ---- TFT rendering ----
// Text area is anchored at the top-left: 80*4 = 320 wide, 25*8 = 200 tall.
static void draw_cell(TFT_eSPI& tft, int r, int c, bool cursor) {
  uint8_t ch  = cell_ch[r][c];
  uint8_t at  = cell_at[r][c];
  uint16_t fg = kPalette[at & 0x0F];
  uint16_t bg = kPalette[(at >> 4) & 0x07];
  if (at & ATTR_INVERSE) {
    uint16_t tmp = fg;
    fg = bg;
    bg = tmp;
  }

  uint16_t buf[4 * 8];
  for (int y = 0; y < 8; y++) {
    uint8_t bits = pgm_read_byte(&font4x8[ch][y]);
    for (int x = 0; x < 4; x++)
      buf[y * 4 + x] = (bits & (1 << x)) ? fg : bg;
  }
  if (cursor) {                          // underline on the bottom two rows
    for (int x = 0; x < 4; x++) { buf[7 * 4 + x] = fg; buf[6 * 4 + x] = fg; }
  }
  tft.pushImage(c * CELL_W, r * CELL_H, CELL_W, CELL_H, buf);
}

void console_render(TFT_eSPI& tft) {
  bool full = !shad_valid;
  for (int r = 0; r < CON_ROWS; r++) {
    for (int c = 0; c < CON_COLS; c++) {
      bool is_cur  = (r == cur_r && c == cur_c);
      bool was_cur = (r == prev_cur_r && c == prev_cur_c);
      bool changed = full ||
                     cell_ch[r][c] != shad_ch[r][c] ||
                     cell_at[r][c] != shad_at[r][c] ||
                     is_cur != was_cur;
      if (changed) {
        draw_cell(tft, r, c, is_cur);
        shad_ch[r][c] = cell_ch[r][c];
        shad_at[r][c] = cell_at[r][c];
      }
    }
  }
  prev_cur_r = cur_r;
  prev_cur_c = cur_c;
  shad_valid = true;
}
