#include "kek_kwp.h"

#include <Arduino.h>

#include "kwp.h"
#include "platform.h"

namespace kek_kwp {

static constexpr uint16_t CSR_ERR    = 0100000;
static constexpr uint16_t CSR_DONE   = 0000200;
static constexpr uint16_t CSR_IE     = 0000100;
static constexpr uint16_t CSR_FIX    = 0000040;
static constexpr uint16_t CSR_UPDN   = 0000020;
static constexpr uint16_t CSR_MODE   = 0000010;
static constexpr uint16_t CSR_RATE   = 0000006;
static constexpr uint16_t CSR_GO     = 0000001;
static constexpr uint16_t CSR_RDMASK = 0100377;
static constexpr uint16_t CSR_WRMASK = 0000137;

static constexpr uint32_t RATE_US[4] = {
    10,       // 100 kHz crystal
    100,      // 10 kHz crystal
    16667,    // line-frequency rate
    100000,   // external input, modeled as 10 Hz
};

static uint16_t csr = 0;
static uint16_t csb = 0;
static uint16_t cntr = 0;
static uint32_t last_us = 0;
static uint32_t remainder_us = 0;
static uint8_t poll_divider = 0;
static bool irq_pending = false;

bool contains(uint16_t addr)
{
  return addr >= CSR_ADDR && addr < END_ADDR;
}

static uint8_t rate_index()
{
  return (uint8_t)((csr & CSR_RATE) >> 1);
}

static void clear_interrupt()
{
  irq_pending = false;
}

static void request_interrupt()
{
  if (csr & CSR_IE)
    irq_pending = true;
}

bool take_interrupt()
{
  if (!kwp::enabled || !irq_pending)
    return false;
  irq_pending = false;
  return true;
}

static uint32_t steps_to_expiry(uint16_t value)
{
  uint32_t steps = (csr & CSR_UPDN)
      ? (0x10000u - (uint32_t)value)
      : (uint32_t)value;
  return steps ? steps : 0x10000u;
}

static void advance_without_expiry(uint32_t steps)
{
  if (csr & CSR_UPDN)
    cntr = (uint16_t)(cntr + steps);
  else
    cntr = (uint16_t)(cntr - steps);
}

static void record_expirations(uint64_t count)
{
  if (!count)
    return;
  if ((csr & CSR_DONE) || count > 1)
    csr |= CSR_ERR;
  csr |= CSR_DONE;
  request_interrupt();
}

static void advance_steps(uint64_t steps)
{
  if (!(csr & CSR_GO) || !steps)
    return;

  uint32_t first = steps_to_expiry(cntr);
  if (steps < first) {
    advance_without_expiry((uint32_t)steps);
    return;
  }

  steps -= first;
  cntr = 0;
  uint64_t expirations = 1;

  if (!(csr & CSR_MODE)) {
    record_expirations(expirations);
    csb = 0;
    csr &= (uint16_t)~CSR_GO;
    remainder_us = 0;
    return;
  }

  cntr = csb;
  uint32_t cycle = steps_to_expiry(cntr);
  if (steps >= cycle) {
    expirations += steps / cycle;
    steps %= cycle;
  }
  if (steps)
    advance_without_expiry((uint32_t)steps);

  record_expirations(expirations);
}

static void update_clock()
{
  uint32_t now = micros();
  uint32_t elapsed = now - last_us;
  last_us = now;

  if (!(csr & CSR_GO)) {
    remainder_us = 0;
    return;
  }

  uint32_t period = RATE_US[rate_index()];
  uint64_t accumulated = (uint64_t)remainder_us + elapsed;
  uint64_t steps = accumulated / period;
  remainder_us = (uint32_t)(accumulated % period);
  advance_steps(steps);
}

static void manual_tick()
{
  if (csr & CSR_GO)
    return;

  if (csr & CSR_UPDN)
    cntr = (uint16_t)(cntr + 1);
  else
    cntr = (uint16_t)(cntr - 1);

  if (cntr == 0) {
    record_expirations(1);
    if (!(csr & CSR_MODE))
      csb = 0;
    else
      cntr = csb;
  }
}

void reset()
{
  csr = 0;
  csb = 0;
  cntr = 0;
  remainder_us = 0;
  poll_divider = 0;
  irq_pending = false;
  last_us = micros();
}

uint16_t read_word(uint16_t addr)
{
  if (!kwp::enabled)
    return 0;

  update_clock();

  switch (addr & ~1u) {
    case CSR_ADDR: {
      uint16_t value = csr & CSR_RDMASK;
      csr &= (uint16_t)~(CSR_ERR | CSR_DONE);
      clear_interrupt();
      return value;
    }
    case CSB_ADDR:
      return 0;
    case CNTR_ADDR:
      return cntr;
    default:
      return 0;
  }
}

uint8_t read_byte(uint16_t addr)
{
  uint16_t value = read_word(addr & ~1u);
  return (addr & 1) ? (uint8_t)(value >> 8) : (uint8_t)value;
}

void write_word(uint16_t addr, uint16_t value)
{
  if (!kwp::enabled)
    return;

  update_clock();

  switch (addr & ~1u) {
    case CSR_ADDR: {
      uint16_t old = csr;
      uint8_t old_rate = rate_index();
      bool was_running = (old & CSR_GO) != 0;

      clear_interrupt();
      csr = value & CSR_WRMASK;

      if (!(csr & CSR_GO)) {
        remainder_us = 0;
        if (value & CSR_FIX)
          manual_tick();
      } else if (!was_running || old_rate != rate_index()) {
        cntr = csb;
        remainder_us = 0;
      }
      last_us = micros();
      break;
    }

    case CSB_ADDR:
      csb = value;
      cntr = value;
      csr &= (uint16_t)~(CSR_ERR | CSR_DONE);
      clear_interrupt();
      remainder_us = 0;
      last_us = micros();
      break;

    case CNTR_ADDR:
    default:
      break;
  }
}

void write_byte(uint16_t addr, uint8_t value)
{
  uint16_t old = 0;
  switch (addr & ~1u) {
    case CSR_ADDR:  old = csr; break;
    case CSB_ADDR:  old = csb; break;
    case CNTR_ADDR: old = cntr; break;
    default:        old = 0; break;
  }
  if (addr & 1)
    old = (uint16_t)((old & 0000377) | ((uint16_t)value << 8));
  else
    old = (uint16_t)((old & 0177400) | value);
  write_word(addr & ~1u, old);
}

void tick()
{
  if (!kwp::enabled)
    return;
  if (++poll_divider >= 16) {
    poll_divider = 0;
    update_clock();
  }
}

}  // namespace kek_kwp
