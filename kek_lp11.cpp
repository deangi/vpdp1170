#include "kek_lp11.h"

#include "lp11_capture.h"

#include <stdint.h>

namespace kek_lp11 {

static constexpr uint16_t LPS_DONE = 0000200;
static constexpr uint16_t LPS_IE   = 0000100;
static constexpr uint32_t PRINT_DELAY_INSTR = 100;

static InstructionCounterFn g_guest_instr_count = nullptr;
static uint16_t lps = LPS_DONE;
static uint16_t lpb = 0;
static uint64_t busy_until_instr = 0;
static bool irq_pending = false;
static bool have_stashed = false;
static uint8_t stashed_char = 0;

void set_instruction_counter(InstructionCounterFn fn) {
  g_guest_instr_count = fn;
}

static uint64_t guest_instr_now() {
  return g_guest_instr_count ? g_guest_instr_count() : 0;
}

bool contains(uint16_t addr) {
  const uint16_t a = (uint16_t)(addr & ~1u);
  return a == CSR_ADDR || a == DB_ADDR;
}

static void clear_interrupt() {
  irq_pending = false;
}

static void request_interrupt_if_ie() {
  if ((lps & LPS_IE) && (lps & LPS_DONE))
    irq_pending = true;
}

static void complete_print() {
  lps |= LPS_DONE;
  busy_until_instr = 0;
  request_interrupt_if_ie();
}

static bool try_accept_char(uint8_t c) {
  if (!lp11_capture::push(c))
    return false;
  lpb = c;
  busy_until_instr = guest_instr_now() + PRINT_DELAY_INSTR;
  return true;
}

void reset() {
  clear_interrupt();
  lps = LPS_DONE;
  lpb = 0;
  busy_until_instr = 0;
  have_stashed = false;
  stashed_char = 0;
  lp11_capture::init();
  lp11_capture::begin_session();
}

void tick() {
  // Retry a char that could not enter the capture FIFO (backpressure).
  if (have_stashed) {
    if (try_accept_char(stashed_char))
      have_stashed = false;
    else
      return;  // still busy / DONE stays clear
  }

  if (!(lps & LPS_DONE) && busy_until_instr != 0) {
    if (guest_instr_now() >= busy_until_instr)
      complete_print();
  }
}

bool take_interrupt() {
  if (!irq_pending)
    return false;
  irq_pending = false;
  return true;
}

uint16_t read_word(uint16_t addr) {
  switch (addr & ~1u) {
    case CSR_ADDR:
      return lps;
    case DB_ADDR:
      return 0;
    default:
      return 0;
  }
}

uint8_t read_byte(uint16_t addr) {
  uint16_t value = read_word(addr & ~1u);
  return (addr & 1) ? (uint8_t)(value >> 8) : (uint8_t)value;
}

void write_word(uint16_t addr, uint16_t value) {
  switch (addr & ~1u) {
    case CSR_ADDR: {
      const bool ie_was = (lps & LPS_IE) != 0;
      if (value & LPS_IE)
        lps |= LPS_IE;
      else {
        lps &= (uint16_t)~LPS_IE;
        clear_interrupt();
      }
      // Rising IE while DONE: INIT "does it interrupt?" probe.
      if (!ie_was && (lps & LPS_IE) && (lps & LPS_DONE))
        irq_pending = true;
      break;
    }
    case DB_ADDR: {
      const uint8_t c = (uint8_t)(value & 0177);
      lps &= (uint16_t)~LPS_DONE;
      clear_interrupt();
      if (!try_accept_char(c)) {
        have_stashed = true;
        stashed_char = c;
        busy_until_instr = 0;
      }
      break;
    }
    default:
      break;
  }
}

void write_byte(uint16_t addr, uint8_t value) {
  uint16_t old = read_word(addr & ~1u);
  if (addr & 1)
    old = (uint16_t)((old & 0000377) | ((uint16_t)value << 8));
  else
    old = (uint16_t)((old & 0177400) | value);
  write_word(addr & ~1u, old);
}

}  // namespace kek_lp11
