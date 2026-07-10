#include "config.h"

#if VPDP1170_USE_KEK_CORE && VPDP1170_BUILD_KEK_ADAPTER

// DEC-faithful KL11/DL11 console for the kek 11/70 path (Option 3).
// Design inputs: EK-DL11-TM-003, open-simh pdp11_stddev.c, vpdp1140 kl11
// reference copies, Phase 2 boot traces. Not derived from the former stub.

#include <stdio.h>
#include <string.h>

#include "_upstream_kek/tty.h"
#include "_upstream_kek/cpu.h"

#include "console.h"
#include "kl11.h"
#include "platform.h"
#include "telnet.h"

#include <Arduino.h>

static constexpr uint16_t TTY_DONE = 0200;   // bit 7
static constexpr uint16_t TTY_IE   = 0100;   // bit 6
static constexpr uint16_t TTY_RDR_ENB = 0001; // WO; clears DONE; reads as 0
static constexpr uint16_t TTY_CSR_RW = TTY_IE;
static constexpr uint16_t TTY_CSR_IMP = (TTY_DONE | TTY_IE);

// Match vpdp1140 host pacing (~32 instruction polls per TX char).
static constexpr uint8_t TTY_TX_POLL_DIV = 32;
static constexpr uint8_t TTY_RX_POLL_DIV = 100;

static uint32_t g_tty_trace_count = 0;
static uint32_t g_tty_tx_chars = 0;
static uint32_t g_tty_tx_ready_events = 0;
static uint32_t g_tty_tx_irq_queues = 0;
static uint32_t g_tty_tx_irq_unqueues = 0;
static uint32_t g_tty_rx_chars = 0;
static uint32_t g_tty_rx_irq_queues = 0;
static uint32_t g_tty_rx_irq_unqueues = 0;
static uint32_t g_tty_last_tx_ms = 0;
static uint32_t g_tty_last_tx_ready_ms = 0;
static uint8_t g_tty_last_tx = 0;
static uint16_t g_tty_tks_snapshot = 0;
static uint16_t g_tty_tkb_snapshot = 0;
static uint16_t g_tty_tps_snapshot = TTY_DONE;
static uint16_t g_tty_tpb_snapshot = 0;
static uint8_t g_tty_tx_busy_snapshot = 0;

static uint32_t s_last_rx_ms = 0;
static bool s_rx_fifo_drained = true;
static uint8_t s_rx_poll_div = 0;
static uint8_t s_tx_poll = 0;

extern "C" void kek_tty_set_trace(uint32_t count) {
  g_tty_trace_count = count;
}

extern "C" uint32_t kek_tty_trace_remaining() {
  return g_tty_trace_count;
}

extern "C" void kek_tty_get_stats(uint32_t* tx_chars, uint32_t* tx_ready_events,
                                  uint32_t* tx_irq_queues,
                                  uint32_t* tx_irq_unqueues,
                                  uint32_t* rx_chars,
                                  uint32_t* rx_irq_queues,
                                  uint32_t* rx_irq_unqueues,
                                  uint8_t* last_tx,
                                  uint32_t* last_tx_ms,
                                  uint32_t* last_tx_ready_ms,
                                  uint32_t* trace_remaining,
                                  uint16_t* tks, uint16_t* tkb,
                                  uint16_t* tps, uint16_t* tpb,
                                  uint8_t* tx_busy) {
  if (tx_chars) *tx_chars = g_tty_tx_chars;
  if (tx_ready_events) *tx_ready_events = g_tty_tx_ready_events;
  if (tx_irq_queues) *tx_irq_queues = g_tty_tx_irq_queues;
  if (tx_irq_unqueues) *tx_irq_unqueues = g_tty_tx_irq_unqueues;
  if (rx_chars) *rx_chars = g_tty_rx_chars;
  if (rx_irq_queues) *rx_irq_queues = g_tty_rx_irq_queues;
  if (rx_irq_unqueues) *rx_irq_unqueues = g_tty_rx_irq_unqueues;
  if (last_tx) *last_tx = g_tty_last_tx;
  if (last_tx_ms) *last_tx_ms = g_tty_last_tx_ms;
  if (last_tx_ready_ms) *last_tx_ready_ms = g_tty_last_tx_ready_ms;
  if (trace_remaining) *trace_remaining = g_tty_trace_count;
  if (tks) *tks = g_tty_tks_snapshot;
  if (tkb) *tkb = g_tty_tkb_snapshot;
  if (tps) *tps = g_tty_tps_snapshot;
  if (tpb) *tpb = g_tty_tpb_snapshot;
  if (tx_busy) *tx_busy = g_tty_tx_busy_snapshot;
}

static const char* tty_reg_name(uint16_t addr) {
  switch (addr & ~1u) {
    case PDP11TTY_TKS: return "TKS";
    case PDP11TTY_TKB: return "TKB";
    case PDP11TTY_TPS: return "TPS";
    case PDP11TTY_TPB: return "TPB";
    default: return "???";
  }
}

// Charge console_trace for guest-visible CSR/data and IRQ *queue* only.
// Unqueue storms must not burn the budget (Phase 2 finding).
static void tty_trace(const char* action, uint16_t addr, uint16_t value,
                      const char* detail = nullptr, bool charge = true) {
  if (g_tty_trace_count == 0) return;
  if (charge) g_tty_trace_count--;
  if (detail && *detail) {
    LOG("kek TTY %-8s %-3s @ %06o val=%06o %s remaining=%u",
        action, tty_reg_name(addr), addr, value, detail,
        (unsigned)g_tty_trace_count);
  } else {
    LOG("kek TTY %-8s %-3s @ %06o val=%06o remaining=%u",
        action, tty_reg_name(addr), addr, value,
        (unsigned)g_tty_trace_count);
  }
}

static void tty_save_snapshot(const uint16_t* registers, bool tx_busy) {
  g_tty_tks_snapshot = registers[(PDP11TTY_TKS - PDP11TTY_BASE) / 2];
  g_tty_tkb_snapshot = registers[(PDP11TTY_TKB - PDP11TTY_BASE) / 2];
  g_tty_tps_snapshot = registers[(PDP11TTY_TPS - PDP11TTY_BASE) / 2];
  g_tty_tpb_snapshot = registers[(PDP11TTY_TPB - PDP11TTY_BASE) / 2];
  g_tty_tx_busy_snapshot = tx_busy ? 1 : 0;
}

static bool pop_host_input(uint8_t* out) {
  uint32_t now = millis();
  bool delay_ok = s_rx_fifo_drained ||
                  (uint32_t)(now - s_last_rx_ms) >= kl11::serial_in_delay_ms;
  if (!delay_ok) return false;

  static bool prefer_telnet = false;
  bool got = false;
  if (prefer_telnet) {
    got = telnet_in_pop(out) || console_key_pop(out);
  } else {
    got = console_key_pop(out) || telnet_in_pop(out);
  }
  if (got) {
    prefer_telnet = !prefer_telnet;
    s_last_rx_ms = now;
    s_rx_fifo_drained = false;
  } else {
    s_rx_fifo_drained = true;
  }
  return got;
}

tty::tty(console *const c, bus *const b) : c(c), b(b) {
  reset(true);
}

tty::~tty() {}

void tty::update_rx_interrupt() {
  const int tks = (PDP11TTY_TKS - PDP11TTY_BASE) / 2;
  const uint16_t csr = registers[tks];
  const bool want = (csr & TTY_CSR_IMP) == TTY_CSR_IMP;

  // SIMH sticky: queue once when DONE∧IE becomes true; unqueue only when
  // the request condition clears. After CPU accepts the vector the sticky
  // flag stays set so idle DONE∧IE does not re-storm.
  if (want) {
    if (!rx_irq_asserted) {
      rx_irq_asserted = true;
      g_tty_rx_irq_queues++;
      tty_trace("IRQ", PDP11TTY_TKS, csr, "queue vec=060");
      if (b && b->getCpu()) b->getCpu()->queue_interrupt(4, 060);
    }
  } else if (rx_irq_asserted) {
    rx_irq_asserted = false;
    g_tty_rx_irq_unqueues++;
    tty_trace("IRQ", PDP11TTY_TKS, csr, "unqueue vec=060", false);
    if (b && b->getCpu()) b->getCpu()->unqueue_interrupt(4, 060);
  }
}

void tty::update_tx_interrupt() {
  const int tps = (PDP11TTY_TPS - PDP11TTY_BASE) / 2;
  const uint16_t csr = registers[tps];
  const bool want = (csr & TTY_CSR_IMP) == TTY_CSR_IMP;

  if (want) {
    if (!tx_irq_asserted) {
      tx_irq_asserted = true;
      tx_ready_reported = true;
      g_tty_tx_irq_queues++;
      tty_trace("IRQ", PDP11TTY_TPS, csr, "queue vec=064");
      if (b && b->getCpu()) b->getCpu()->queue_interrupt(4, 064);
    }
  } else if (tx_irq_asserted) {
    tx_irq_asserted = false;
    g_tty_tx_irq_unqueues++;
    tty_trace("IRQ", PDP11TTY_TPS, csr, "unqueue vec=064", false);
    if (b && b->getCpu()) b->getCpu()->unqueue_interrupt(4, 064);
  }
}

void tty::notify_rx() {
  const int tks = (PDP11TTY_TKS - PDP11TTY_BASE) / 2;
  const int tkb = (PDP11TTY_TKB - PDP11TTY_BASE) / 2;
  if (registers[tks] & TTY_DONE) return;

  uint8_t ch = 0;
  if (!pop_host_input(&ch)) return;

  registers[tkb] = ch & 0177;
  registers[tks] = (registers[tks] & TTY_IE) | TTY_DONE;
  g_tty_rx_chars++;
  tty_trace("RXREADY", PDP11TTY_TKB, registers[tkb]);
  update_rx_interrupt();
  tty_save_snapshot(registers, tx_busy);
}

void tty::service_deferred() {
  const int tps = (PDP11TTY_TPS - PDP11TTY_BASE) / 2;

  if (tx_busy) {
    if (++s_tx_poll >= TTY_TX_POLL_DIV) {
      s_tx_poll = 0;
      tx_busy = false;
      registers[tps] |= TTY_DONE;
      g_tty_tx_ready_events++;
      g_tty_last_tx_ready_ms = millis();
      tty_trace("TXREADY", PDP11TTY_TPS, registers[tps]);
      // Rising DONE while IE set → new sticky request (SIMH SET_INT).
      tx_irq_asserted = false;
      update_tx_interrupt();
      tty_save_snapshot(registers, tx_busy);
    }
  } else if (!(registers[tps] & TTY_DONE)) {
    registers[tps] |= TTY_DONE;
    tx_irq_asserted = false;
    update_tx_interrupt();
  }

  if (++s_rx_poll_div >= TTY_RX_POLL_DIV) {
    s_rx_poll_div = 0;
    notify_rx();
  }
}

void tty::reset(const bool hard) {
  if (hard) {
    memset(registers, 0, sizeof(registers));
  }
  // INIT sets transmitter ready (DEC).
  registers[(PDP11TTY_TPS - PDP11TTY_BASE) / 2] = TTY_DONE;
  registers[(PDP11TTY_TKS - PDP11TTY_BASE) / 2] = 0;
  rx_irq_asserted = false;
  tx_irq_asserted = false;
  tx_ready_reported = true;
  tx_busy = false;
  tx_ready_at_us = 0;
  s_tx_poll = 0;
  s_rx_poll_div = 0;
  s_rx_fifo_drained = true;
  g_tty_tx_chars = 0;
  g_tty_tx_ready_events = 0;
  g_tty_tx_irq_queues = 0;
  g_tty_tx_irq_unqueues = 0;
  g_tty_rx_chars = 0;
  g_tty_rx_irq_queues = 0;
  g_tty_rx_irq_unqueues = 0;
  g_tty_last_tx_ms = 0;
  g_tty_last_tx_ready_ms = 0;
  g_tty_last_tx = 0;
  if (b && b->getCpu()) {
    b->getCpu()->unqueue_interrupt(4, 060);
    b->getCpu()->unqueue_interrupt(4, 064);
  }
  tty_save_snapshot(registers, tx_busy);
}

uint8_t tty::read_byte(const uint16_t addr) {
  uint16_t v = read_word(addr & ~1);
  return (addr & 1) ? (uint8_t)(v >> 8) : (uint8_t)v;
}

uint16_t tty::read_word(const uint16_t addr) {
  const int reg = (addr - PDP11TTY_BASE) / 2;
  if (reg < 0 || reg >= 4) return 0;

  if (addr == PDP11TTY_TKS) {
    // Opportunistic RX load on status poll (boot ROMs / polled drivers).
    notify_rx();
    uint16_t value = registers[reg] & TTY_CSR_IMP;
    tty_trace("READ", addr, value);
    tty_save_snapshot(registers, tx_busy);
    return value;
  }

  if (addr == PDP11TTY_TKB) {
    uint16_t value = registers[reg] & 0177;
    registers[(PDP11TTY_TKS - PDP11TTY_BASE) / 2] &= (uint16_t)~TTY_DONE;
    update_rx_interrupt();
    tty_trace("READ", addr, value);
    tty_save_snapshot(registers, tx_busy);
    return value;
  }

  if (addr == PDP11TTY_TPS) {
    // Status only — no IRQ side effects (SIMH / DEC; RT-11 polls TPS).
    uint16_t value = registers[reg] & TTY_CSR_IMP;
    tty_trace("READ", addr, value);
    return value;
  }

  tty_trace("READ", addr, registers[reg] & 0377);
  return registers[reg] & 0377;
}

void tty::write_byte(const uint16_t addr, const uint8_t v) {
  // High-byte DATOB ignored on KL11/DL11-A/B status (DEC).
  if (addr & 1) return;
  uint16_t value = registers[(addr - PDP11TTY_BASE) / 2];
  value = (value & 0177400) | v;
  write_word(addr & ~1, value);
}

void tty::write_word(const uint16_t addr, uint16_t v) {
  const int reg = (addr - PDP11TTY_BASE) / 2;
  if (reg < 0 || reg >= 4) return;

  if (addr == PDP11TTY_TKS) {
    // SIMH: IE only in RW mask; RDR ENB (bit 0) clears DONE (DEC).
    const bool rdr_enb = (v & TTY_RDR_ENB) != 0;
    uint16_t csr = registers[reg];
    if (rdr_enb) csr &= (uint16_t)~TTY_DONE;
    // IE write: clearing IE drops request; enabling IE while DONE set raises.
    if ((v & TTY_IE) == 0) {
      csr &= (uint16_t)~TTY_IE;
    } else {
      if ((csr & (TTY_DONE | TTY_IE)) == TTY_DONE)
        rx_irq_asserted = false;  // force re-evaluate → queue
      csr |= TTY_IE;
    }
    registers[reg] = csr & TTY_CSR_IMP;
    tty_trace("WRITE", addr, registers[reg]);
    update_rx_interrupt();
    notify_rx();
    tty_save_snapshot(registers, tx_busy);
    return;
  }

  if (addr == PDP11TTY_TPS) {
    uint16_t csr = registers[reg];
    if ((v & TTY_IE) == 0) {
      csr &= (uint16_t)~TTY_IE;
    } else {
      if ((csr & (TTY_DONE | TTY_IE)) == TTY_DONE)
        tx_irq_asserted = false;
      csr |= TTY_IE;
    }
    // Preserve READY (bit 7); never invent high-byte mirrors.
    registers[reg] = (csr & TTY_DONE) | (csr & TTY_IE);
    tty_trace("WRITE", addr, registers[reg]);
    update_tx_interrupt();
    tty_save_snapshot(registers, tx_busy);
    return;
  }

  if (addr == PDP11TTY_TPB) {
    uint8_t out = v & 0177;
    registers[reg] = out;
    tx_busy = true;
    s_tx_poll = 0;
    registers[(PDP11TTY_TPS - PDP11TTY_BASE) / 2] &= (uint16_t)~TTY_DONE;
    update_tx_interrupt();

    console_feed(out);
    telnet_write(out);
    if (!g_serial_silenced) kl11::queue_serial_out(out);

    g_tty_tx_chars++;
    g_tty_last_tx = out;
    g_tty_last_tx_ms = millis();
    char detail[24];
    snprintf(detail, sizeof(detail), "ch=%03o", (unsigned)out);
    tty_trace("WRITE", addr, out, detail);
    tty_save_snapshot(registers, tx_busy);
    return;
  }

  registers[reg] = v;
  tty_save_snapshot(registers, tx_busy);
}

void tty::operator()() {}

#endif
