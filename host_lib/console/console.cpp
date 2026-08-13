#include "console.h"
#include "console_internal.h"
#include "term_adm3a.h"
#include "config.h"
#include "platform.h"
#include "gfx.h"
#if VPDP_BOARD == VPDP_BOARD_CROWPANEL_7
#include "font10x16.h"   // Terminus 8x16 in 10x16 cells (80x25 -> 800x400)
#else
#include "font4x8.h"     // Freenove 4x8 cells (80x25 -> 320x200)
#endif
#include "fifo.h"
#include <Arduino.h>
#include <atomic>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

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

// Cell grid + cursor are written by the core-0 TFT output task and read by
// the core-0 render task. Without a coherent snapshot, a cursor advance
// mid-frame leaves the underline behind, and a scroll mid-pass leaves
// mismatched shadow cells (artifacts / missing glyphs).
static portMUX_TYPE g_con_mux = portMUX_INITIALIZER_UNLOCKED;

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
//      TFT-output FIFO. Guest KL11 backpressure reserves enough room for the
//      next character. Payload and atomic cursors live in internal RAM. ----
#define VPDP_SERIAL_IN_FIFO_BYTES 8192
#define VPDP_TFT_OUT_FIFO_BYTES 8192
static uint8_t serial_in_storage[VPDP_SERIAL_IN_FIFO_BYTES];
static uint8_t tft_out_storage[VPDP_TFT_OUT_FIFO_BYTES];
static Fifo g_serial_in;
static Fifo g_tft_out;
static TaskHandle_t g_tft_output_task = nullptr;
static std::atomic<uint32_t> g_tft_dropped { 0 };
static std::atomic<bool> g_tft_reset_requested { false };

// ---- output activity ----
static std::atomic<uint32_t> g_feed_count   { 0 };
static std::atomic<uint32_t> g_last_feed_ms { 0 };
static HostTermPersonality g_personality = HOST_TERM_VT100;

// -------------------------------------------------------------------------

static void clear_row(int r, uint8_t attr) {
  for (int c = 0; c < CON_COLS; c++) { cell_ch[r][c] = ' '; cell_at[r][c] = attr; }
}

void console_set_personality(HostTermPersonality p) {
  g_personality = p;
  term_adm3a_reset();
  ansi_st = ST_GROUND;
  csi_private = false;
}

HostTermPersonality console_personality() { return g_personality; }

void console_init() {
  if (g_tft_output_task) {
    g_tft_reset_requested.store(true, std::memory_order_release);
    xTaskNotifyGive(g_tft_output_task);
    while (g_tft_reset_requested.load(std::memory_order_acquire))
      vTaskDelay(1);
  } else {
    g_tft_out.init(tft_out_storage, sizeof(tft_out_storage));
  }

  portENTER_CRITICAL(&g_con_mux);
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
  term_adm3a_reset();
  shad_valid = false;
  g_serial_in.init(serial_in_storage, VPDP_SERIAL_IN_FIFO_BYTES);
  g_tft_dropped.store(0, std::memory_order_relaxed);
  g_feed_count.store(0, std::memory_order_relaxed);
  g_last_feed_ms.store(0, std::memory_order_relaxed);
  portEXIT_CRITICAL(&g_con_mux);
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

void con_put_glyph(uint8_t ch) { put_glyph(ch); }
void con_bs() { if (cur_c > 0) cur_c--; }
void con_tab() {
  cur_c = (cur_c + 8) & ~7;
  if (cur_c >= CON_COLS) cur_c = CON_COLS - 1;
}
void con_lf() { cursor_down_scroll(); }
void con_cr() { cur_c = 0; }
void con_home() { cur_r = 0; cur_c = 0; }
void con_clear_screen() {
  for (int r = 0; r < CON_ROWS; r++) clear_row(r, cur_attr);
}
void con_cursor_up(int n) { if (n < 1) n = 1; cur_r -= n; clamp_cursor(); }
void con_cursor_down(int n) { if (n < 1) n = 1; cur_r += n; clamp_cursor(); }
void con_cursor_left(int n) { if (n < 1) n = 1; cur_c -= n; clamp_cursor(); }
void con_cursor_right(int n) { if (n < 1) n = 1; cur_c += n; clamp_cursor(); }
void con_cursor_set(int r, int c) { cur_r = r; cur_c = c; clamp_cursor(); }
void con_erase_eol() { erase_in_line(0); }
int con_cols() { return CON_COLS; }
int con_rows() { return CON_ROWS; }

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
// Internal ANSI-parser entrypoint. Called by the TFT output task under
// g_con_mux so console_render can take a coherent grid+cursor snapshot.
static void feed_ansi(uint8_t c) {
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

// ---- TFT-out FIFO: KEK/core 1 push, dedicated core-0 task drain ----
void console_feed(uint8_t c) {
  bool was_empty = false;
  if (!g_tft_out.push(c, &was_empty)) {
    g_tft_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // Wake only on an observed empty->nonempty transition. The consumer also
  // has a short timeout, which closes the harmless observation race without
  // imposing a task-notify call on every guest character.
  if (was_empty && g_tft_output_task)
    xTaskNotifyGive(g_tft_output_task);
}

static size_t drain_tft_output(size_t limit) {
  // Apply under the console mux in small batches so render can take a
  // coherent snapshot between batches without holding a long critical.
  uint8_t batch[64];
  size_t total = 0;
  while (total < limit) {
    size_t n = 0;
    size_t room = limit - total;
    size_t target = room < sizeof(batch) ? room : sizeof(batch);
    while (n < target && g_tft_out.pop(&batch[n])) n++;
    if (!n) break;
    // Activity accounting is per drained batch. This preserves the byte
    // count and idle-time semantics while avoiding millis() on every byte.
    g_feed_count.fetch_add((uint32_t)n, std::memory_order_relaxed);
    g_last_feed_ms.store(millis(), std::memory_order_relaxed);
    portENTER_CRITICAL(&g_con_mux);
    for (size_t i = 0; i < n; i++) {
      if (g_personality == HOST_TERM_ADM3A) term_adm3a_feed(batch[i]);
      else feed_ansi(batch[i]);
    }
    portEXIT_CRITICAL(&g_con_mux);
    total += n;
  }
  return total;
}

static void tft_output_task(void*) {
  for (;;) {
    if (g_tft_reset_requested.load(std::memory_order_acquire)) {
      g_tft_out.clear();
      g_tft_reset_requested.store(false, std::memory_order_release);
    }
    size_t drained = drain_tft_output(2048);
    if (drained == 0)
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
    else
      taskYIELD();
  }
}

bool console_start_output_task() {
  if (g_tft_output_task) return true;
  return xTaskCreatePinnedToCore(tft_output_task, "tftout", 4096, nullptr, 1,
                                 &g_tft_output_task, 0) == pdPASS;
}

void console_output_stats(uint32_t* pending, uint32_t* dropped) {
  if (pending) *pending = (uint32_t)g_tft_out.count();
  if (dropped)
    *dropped = g_tft_dropped.load(std::memory_order_relaxed);
}

bool console_output_has_space(size_t bytes) {
  return g_tft_out.free_space() >= bytes;
}

// ---- keyboard (host USB-Serial -> KL11 input FIFO) ----
void console_key_push(uint8_t c) { g_serial_in.push(c); }
int  console_key_pop(uint8_t* out) { return g_serial_in.pop(out) ? 1 : 0; }

uint32_t console_feed_count() {
  return g_feed_count.load(std::memory_order_relaxed);
}
uint32_t console_last_feed_ms() {
  return g_last_feed_ms.load(std::memory_order_relaxed);
}

// ---- TFT rendering ----
// Text area is anchored top-left: CON_COLS*CELL_W wide, CON_ROWS*CELL_H tall.
// Freenove: 4x8 cells (320x200). CrowPanel: 10x16 cells (800x400).
static void draw_cell(GfxDisplay& tft, int r, int c,
                      uint8_t ch, uint8_t at, bool cursor) {
  uint16_t fg = kPalette[at & 0x0F];
  uint16_t bg = kPalette[(at >> 4) & 0x07];
  if (at & ATTR_INVERSE) {
    uint16_t tmp = fg;
    fg = bg;
    bg = tmp;
  }

  uint16_t buf[CELL_W * CELL_H];
#if VPDP_BOARD == VPDP_BOARD_CROWPANEL_7
  // font10x16[ch][y] is a uint16_t row; bit 0 = leftmost pixel.
  for (int y = 0; y < CELL_H; y++) {
    uint16_t bits = pgm_read_word(&font10x16[ch][y]);
    for (int x = 0; x < CELL_W; x++)
      buf[y * CELL_W + x] = (bits & (1u << x)) ? fg : bg;
  }
#else
  // font4x8[ch][y] is a byte row; bit 0 = leftmost pixel.
  for (int y = 0; y < CELL_H; y++) {
    uint8_t bits = pgm_read_byte(&font4x8[ch][y]);
    for (int x = 0; x < CELL_W; x++)
      buf[y * CELL_W + x] = (bits & (1 << x)) ? fg : bg;
  }
#endif
  if (cursor) {                          // underline on the bottom two rows
    for (int x = 0; x < CELL_W; x++) {
      buf[(CELL_H - 1) * CELL_W + x] = fg;
      buf[(CELL_H - 2) * CELL_W + x] = fg;
    }
  }
  tft.pushImage(c * CELL_W, r * CELL_H, CELL_W, CELL_H, buf);
}

void console_render(GfxDisplay& tft) {
  // Snapshot the live grid + cursor under the mux, then render unlocked.
  // Using a frozen cursor for the whole pass (and for prev_cur update) is
  // what stops the "underline left behind" race; the grid copy is what
  // stops scroll mid-pass shadow mismatches.
  static uint8_t snap_ch[CON_ROWS][CON_COLS];
  static uint8_t snap_at[CON_ROWS][CON_COLS];
  int snap_r, snap_c;
  portENTER_CRITICAL(&g_con_mux);
  memcpy(snap_ch, cell_ch, sizeof snap_ch);
  memcpy(snap_at, cell_at, sizeof snap_at);
  snap_r = cur_r;
  snap_c = cur_c;
  portEXIT_CRITICAL(&g_con_mux);

  bool full = !shad_valid;
  bool any_changed = false;
  int dirty_x0 = CON_COLS * CELL_W, dirty_y0 = CON_ROWS * CELL_H;
  int dirty_x1 = 0, dirty_y1 = 0;

  for (int r = 0; r < CON_ROWS; r++) {
    for (int c = 0; c < CON_COLS; c++) {
      bool is_cur  = (r == snap_r && c == snap_c);
      bool was_cur = (r == prev_cur_r && c == prev_cur_c);
      bool changed = full ||
                     snap_ch[r][c] != shad_ch[r][c] ||
                     snap_at[r][c] != shad_at[r][c] ||
                     is_cur != was_cur;
      if (changed) {
        draw_cell(tft, r, c, snap_ch[r][c], snap_at[r][c], is_cur);
        shad_ch[r][c] = snap_ch[r][c];
        shad_at[r][c] = snap_at[r][c];
        any_changed = true;
        const int px = c * CELL_W, py = r * CELL_H;
        if (px < dirty_x0) dirty_x0 = px;
        if (py < dirty_y0) dirty_y0 = py;
        if (px + CELL_W > dirty_x1) dirty_x1 = px + CELL_W;
        if (py + CELL_H > dirty_y1) dirty_y1 = py + CELL_H;
      }
    }
  }
  prev_cur_r = snap_r;
  prev_cur_c = snap_c;
  shad_valid = true;
  // RGB panels: flush only the dirty bbox to PSRAM (no-op on SPI).
  if (any_changed)
    gfx_writeback(tft, dirty_x0, dirty_y0,
                  dirty_x1 - dirty_x0, dirty_y1 - dirty_y0);
}
