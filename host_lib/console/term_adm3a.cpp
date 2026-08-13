#include "term_adm3a.h"
#include "console_internal.h"

// Lear Siegler ADM-3A (and ADM-3A+ subset used by CP/M). No CSI / VT100.

enum AdmState : uint8_t {
  ADM_GROUND = 0,
  ADM_ESC,
  ADM_EQ_ROW,
  ADM_EQ_COL,
};

static AdmState g_st = ADM_GROUND;
static int g_eq_row = 0;

void term_adm3a_reset() {
  g_st = ADM_GROUND;
  g_eq_row = 0;
}

void term_adm3a_feed(uint8_t c) {
  switch (g_st) {
    case ADM_GROUND:
      switch (c) {
        case 0x07: break;                         // BEL
        case 0x08: con_bs(); break;
        case 0x0A: con_lf(); break;
        case 0x0B: con_cursor_up(1); break;       // VT
        case 0x0C: con_cursor_right(1); break;    // FF = cursor right
        case 0x0D: con_cr(); break;
        case 0x1A: con_clear_screen(); con_home(); break;  // SUB
        case 0x1E: con_home(); break;             // RS
        case 0x1B: g_st = ADM_ESC; break;
        default:
          if (c >= 0x20) con_put_glyph(c);
          break;
      }
      break;

    case ADM_ESC:
      if (c == '=') {
        g_st = ADM_EQ_ROW;
      } else if (c == '*' || c == '+') {
        con_clear_screen();
        con_home();
        g_st = ADM_GROUND;
      } else if (c == 'T' || c == 't') {
        con_erase_eol();
        g_st = ADM_GROUND;
      } else {
        g_st = ADM_GROUND;
      }
      break;

    case ADM_EQ_ROW:
      g_eq_row = (int)c - 32;
      g_st = ADM_EQ_COL;
      break;

    case ADM_EQ_COL:
      con_cursor_set(g_eq_row, (int)c - 32);
      g_st = ADM_GROUND;
      break;
  }
}
