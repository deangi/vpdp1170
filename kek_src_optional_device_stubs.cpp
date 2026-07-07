#include "config.h"

#if VPDP1170_USE_KEK_CORE && VPDP1170_BUILD_KEK_ADAPTER

// Temporary link stubs for kek peripherals that are referenced by bus.cpp but
// are not part of the first ESP32 bring-up slice. Replace these with real
// kek_src_<device>.cpp wrappers one device at a time as each device is ported.

#include <stdio.h>
#include <string.h>

#include "_upstream_kek/dc11.h"
#include "_upstream_kek/deqna.h"
#include "_upstream_kek/dz11.h"
#include "_upstream_kek/rk05.h"
#include "_upstream_kek/rl02.h"
#include "_upstream_kek/rp06.h"
#include "_upstream_kek/tm-11.h"
#include "_upstream_kek/tty.h"
#include "_upstream_kek/cpu.h"

#include "console.h"
#include "platform.h"
#include "telnet.h"

static constexpr uint16_t TTY_DONE = 0200;
static constexpr uint16_t TTY_IE = 0100;
static constexpr uint32_t TTY_TX_DELAY_US = 100;
static constexpr uint32_t TTY_SERIAL_WRITE_WAIT_US = 20000;

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

#define TTY_SAVE_SNAPSHOT() \
  do { \
    g_tty_tks_snapshot = registers[(PDP11TTY_TKS - PDP11TTY_BASE) / 2]; \
    g_tty_tkb_snapshot = registers[(PDP11TTY_TKB - PDP11TTY_BASE) / 2]; \
    g_tty_tps_snapshot = registers[(PDP11TTY_TPS - PDP11TTY_BASE) / 2]; \
    g_tty_tpb_snapshot = registers[(PDP11TTY_TPB - PDP11TTY_BASE) / 2]; \
    g_tty_tx_busy_snapshot = tx_busy ? 1 : 0; \
  } while (0)

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

static void tty_trace(const char* action, uint16_t addr, uint16_t value,
                      const char* detail = nullptr) {
  if (g_tty_trace_count == 0) return;
  g_tty_trace_count--;
  if (detail && *detail) {
    LOG("kek TTY %-8s %-3s @ %06o val=%06o %s remaining=%u",
        action, tty_reg_name(addr), addr, value, detail,
        (unsigned)g_tty_trace_count);
  } else {
    LOG("kek TTY %-8s %-3s @ %06o val=%06o remaining=%u",
        action, tty_reg_name(addr), addr, value,
        (unsigned)g_tty_trace_count);
  }
  if (g_tty_trace_count == 0) {
    LOG("kek TTY trace ended: tx=%u txready=%u irq64 q/u=%u/%u "
        "rx=%u irq60 q/u=%u/%u TKS=%06o TKB=%06o TPS=%06o TPB=%06o "
        "busy=%u last_tx=%03o last_tx_ms=%u last_ready_ms=%u",
        (unsigned)g_tty_tx_chars, (unsigned)g_tty_tx_ready_events,
        (unsigned)g_tty_tx_irq_queues, (unsigned)g_tty_tx_irq_unqueues,
        (unsigned)g_tty_rx_chars, (unsigned)g_tty_rx_irq_queues,
        (unsigned)g_tty_rx_irq_unqueues, (unsigned)g_tty_tks_snapshot,
        (unsigned)g_tty_tkb_snapshot, (unsigned)g_tty_tps_snapshot,
        (unsigned)g_tty_tpb_snapshot, (unsigned)g_tty_tx_busy_snapshot,
        (unsigned)g_tty_last_tx, (unsigned)g_tty_last_tx_ms,
        (unsigned)g_tty_last_tx_ready_ms);
  }
}

static void write_guest_serial_byte(uint8_t out) {
  if (g_serial_silenced) return;

  uint32_t start = micros();
  while (Serial.availableForWrite() <= 0) {
    if ((uint32_t)(micros() - start) >= TTY_SERIAL_WRITE_WAIT_US)
      return;
    delayMicroseconds(50);
  }
  Serial.write(out);
}

static bool pop_kek_console_input(uint8_t* out) {
  static bool prefer_telnet = false;
  bool got = false;
  if (prefer_telnet) {
    got = telnet_in_pop(out) || console_key_pop(out);
  } else {
    got = console_key_pop(out) || telnet_in_pop(out);
  }
  if (got) prefer_telnet = !prefer_telnet;
  return got;
}

tty::tty(console *const c, bus *const b) : c(c), b(b) {}
tty::~tty() {}

void tty::update_rx_interrupt() {
  int tks = (PDP11TTY_TKS - PDP11TTY_BASE) / 2;
  bool should_assert = (registers[tks] & (TTY_DONE | TTY_IE)) == (TTY_DONE | TTY_IE);
  if (should_assert && !rx_irq_asserted) {
    rx_irq_asserted = true;
    g_tty_rx_irq_queues++;
    tty_trace("IRQ", PDP11TTY_TKS, registers[tks], "queue vec=060");
    if (b && b->getCpu()) b->getCpu()->queue_interrupt(4, 060);
  }
  else if (!should_assert && rx_irq_asserted) {
    rx_irq_asserted = false;
    g_tty_rx_irq_unqueues++;
    tty_trace("IRQ", PDP11TTY_TKS, registers[tks], "unqueue vec=060");
    if (b && b->getCpu()) b->getCpu()->unqueue_interrupt(4, 060);
  }
  TTY_SAVE_SNAPSHOT();
}

void tty::update_tx_interrupt() {
  int tps = (PDP11TTY_TPS - PDP11TTY_BASE) / 2;
  bool should_assert = (registers[tps] & (TTY_DONE | TTY_IE)) == (TTY_DONE | TTY_IE);
  bool cpu_has_irq = false;
  if (b && b->getCpu()) {
    cpu_has_irq = b->getCpu()->has_queued_interrupt(4, 064);
  }
  if (should_assert && !cpu_has_irq &&
      (!tx_irq_asserted || !tx_ready_reported)) {
    tx_irq_asserted = true;
    tx_ready_reported = true;
    g_tty_tx_irq_queues++;
    tty_trace("IRQ", PDP11TTY_TPS, registers[tps], "queue vec=064");
    if (b && b->getCpu()) b->getCpu()->queue_interrupt(4, 064);
  }
  else if (should_assert && tx_irq_asserted && !cpu_has_irq) {
    // kek consumes queued vectors when delivered. The DL/KL transmitter ready
    // condition is level-like from the guest's point of view: if DONE+IE is
    // still true after the CPU accepted the previous vector, keep presenting
    // vec 064 until the guest clears IE or writes TPB.
    g_tty_tx_irq_queues++;
    tty_trace("IRQ", PDP11TTY_TPS, registers[tps], "requeue vec=064");
    if (b && b->getCpu()) b->getCpu()->queue_interrupt(4, 064);
  }
  else if (!should_assert && tx_irq_asserted) {
    tx_irq_asserted = false;
    g_tty_tx_irq_unqueues++;
    tty_trace("IRQ", PDP11TTY_TPS, registers[tps], "unqueue vec=064");
    if (b && b->getCpu()) b->getCpu()->unqueue_interrupt(4, 064);
  }
  else if (!should_assert) {
    // Keep the device's local state and kek's set-based interrupt queue in
    // sync even if the CPU accepted the previous vector before we observed the
    // guest clearing IE or DONE.
    tx_irq_asserted = false;
    if (b && b->getCpu()) b->getCpu()->unqueue_interrupt(4, 064);
  }
  TTY_SAVE_SNAPSHOT();
}

void tty::service_deferred() {
  if (!tx_busy) {
    registers[(PDP11TTY_TPS - PDP11TTY_BASE) / 2] |= TTY_DONE;
    TTY_SAVE_SNAPSHOT();
    update_tx_interrupt();
    return;
  }
  if ((int32_t)(micros() - tx_ready_at_us) < 0) return;

  tx_busy = false;
  registers[(PDP11TTY_TPS - PDP11TTY_BASE) / 2] |= TTY_DONE;
  g_tty_tx_ready_events++;
  g_tty_last_tx_ready_ms = millis();
  tty_trace("TXREADY", PDP11TTY_TPS,
            registers[(PDP11TTY_TPS - PDP11TTY_BASE) / 2]);
  TTY_SAVE_SNAPSHOT();
  update_tx_interrupt();
}

void tty::notify_rx() {
  int tks = (PDP11TTY_TKS - PDP11TTY_BASE) / 2;
  int tkb = (PDP11TTY_TKB - PDP11TTY_BASE) / 2;
  if ((registers[tks] & TTY_DONE) == 0) {
    uint8_t ch = 0;
    if (!pop_kek_console_input(&ch)) return;
    registers[tkb] = ch & 0177;
    registers[tks] |= TTY_DONE;
    g_tty_rx_chars++;
    tty_trace("RXREADY", PDP11TTY_TKB, registers[tkb]);
  }
  TTY_SAVE_SNAPSHOT();
  update_rx_interrupt();
}
void tty::reset(const bool hard) {
  if (hard) {
    memset(registers, 0, sizeof(registers));
  }
  registers[(PDP11TTY_TPS - PDP11TTY_BASE) / 2] = TTY_DONE;
  rx_irq_asserted = false;
  tx_irq_asserted = false;
  tx_ready_reported = true;
  tx_busy = false;
  tx_ready_at_us = 0;
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
  TTY_SAVE_SNAPSHOT();
}
uint8_t tty::read_byte(const uint16_t addr) {
  uint16_t v = read_word(addr & ~1);
  return (addr & 1) ? (uint8_t)(v >> 8) : (uint8_t)v;
}
uint16_t tty::read_word(const uint16_t addr) {
  service_deferred();
  int reg = (addr - PDP11TTY_BASE) / 2;
  if (reg < 0 || reg >= 4) return 0;

  if (addr == PDP11TTY_TKS) {
    notify_rx();
    tty_trace("READ", addr, registers[reg]);
    return registers[reg];
  }

  if (addr == PDP11TTY_TKB) {
    uint16_t value = registers[reg] & 0177;
    registers[(PDP11TTY_TKS - PDP11TTY_BASE) / 2] &= ~TTY_DONE;
    update_rx_interrupt();
    notify_rx();
    tty_trace("READ", addr, value);
    TTY_SAVE_SNAPSHOT();
    return value;
  }

  if (addr == PDP11TTY_TPS) {
    if (!tx_busy) {
      registers[reg] |= TTY_DONE;
      TTY_SAVE_SNAPSHOT();
      update_tx_interrupt();
    }
    tty_trace("READ", addr, registers[reg]);
    return registers[reg];
  }

  tty_trace("READ", addr, registers[reg]);
  return registers[reg];
}
void tty::write_byte(const uint16_t addr, const uint8_t v) {
  uint16_t value = registers[(addr - PDP11TTY_BASE) / 2];
  if (addr & 1) {
    value = (value & 000377) | ((uint16_t)v << 8);
  } else {
    value = (value & 0177400) | v;
  }
  write_word(addr & ~1, value);
}
void tty::write_word(const uint16_t addr, uint16_t v) {
  int reg = (addr - PDP11TTY_BASE) / 2;
  if (reg < 0 || reg >= 4) return;

  if (addr == PDP11TTY_TKS) {
    registers[reg] = (registers[reg] & TTY_DONE) | (v & TTY_IE);
    tty_trace("WRITE", addr, registers[reg]);
    notify_rx();
    TTY_SAVE_SNAPSHOT();
    return;
  }

  if (addr == PDP11TTY_TPS) {
    bool old_ie = (registers[reg] & TTY_IE) != 0;
    if (!tx_busy) {
      registers[reg] |= TTY_DONE;
    }
    registers[reg] = (registers[reg] & TTY_DONE) | (v & TTY_IE);
    if (!old_ie && (registers[reg] & (TTY_DONE | TTY_IE)) == (TTY_DONE | TTY_IE)) {
      tx_ready_reported = false;
    }
    tty_trace("WRITE", addr, registers[reg]);
    update_tx_interrupt();
    TTY_SAVE_SNAPSHOT();
    return;
  }

  if (addr == PDP11TTY_TPB) {
    uint8_t out = v & 0177;
    tx_busy = true;
    tx_ready_reported = false;
    tx_ready_at_us = micros() + TTY_TX_DELAY_US;
    registers[(PDP11TTY_TPS - PDP11TTY_BASE) / 2] &= ~TTY_DONE;
    update_tx_interrupt();

    console_feed(out);
    telnet_write(out);
    write_guest_serial_byte(out);
    g_tty_tx_chars++;
    g_tty_last_tx = out;
    g_tty_last_tx_ms = millis();
    char detail[24];
    snprintf(detail, sizeof(detail), "ch=%03o", (unsigned)out);
    tty_trace("WRITE", addr, out, detail);
  }

  registers[reg] = v;
  TTY_SAVE_SNAPSHOT();
}
void tty::operator()() {}

rp06::rp06(bus *const b, abool *const disk_read_activity, abool *const disk_write_activity, const bool is_rp07)
  : b(b), is_rp07(is_rp07), disk_read_activity(disk_read_activity), disk_write_activity(disk_write_activity) {}
rp06::~rp06() {}
void rp06::begin() {}
void rp06::reset(const bool) {}
void rp06::show_state(console *const) const {}
uint8_t rp06::read_byte(const uint16_t) { return 0; }
uint16_t rp06::read_word(const uint16_t) { return 0; }
void rp06::write_byte(const uint16_t, const uint8_t) {}
void rp06::write_word(const uint16_t, const uint16_t) {}

dc11::dc11(bus *const b, comm_io *const io_channels) : b(b), io_channels(io_channels) {}
dc11::~dc11() {}
bool dc11::begin() { return true; }
void dc11::reset(const bool) {}
void dc11::show_state(console *const) const {}
void dc11::test_port(const size_t, const std::string &) const {}
void dc11::test_ports(const std::string &) const {}
uint8_t dc11::read_byte(const uint16_t) { return 0; }
uint16_t dc11::read_word(const uint16_t) { return 0; }
void dc11::write_byte(const uint16_t, const uint8_t) {}
void dc11::write_word(const uint16_t, const uint16_t) {}
void dc11::operator()() {}

dz11::dz11(bus *const b, comm_io *const io_channels) : b(b), io_channels(io_channels) {}
dz11::~dz11() {}
bool dz11::begin() { return true; }
void dz11::reset(const bool) {}
void dz11::show_state(console *const) const {}
void dz11::test_port(const size_t) const {}
void dz11::test_ports(const int) const {}
uint8_t dz11::read_byte(const uint16_t) { return 0; }
uint16_t dz11::read_word(const uint16_t) { return 0; }
void dz11::write_byte(const uint16_t, const uint8_t) {}
void dz11::write_word(const uint16_t, const uint16_t) {}
void dz11::operator()() {}

deqna::deqna(bus *const b, const uint8_t mac_address[6], eth_transport *const eth_dev, abool *const activity_flag)
  : b(b), eth_dev(eth_dev), activity_flag(activity_flag) {
  if (mac_address) {
    memcpy(this->mac_address, mac_address, sizeof(this->mac_address));
  }
}
deqna::~deqna() {}
bool deqna::begin() { return true; }
void deqna::receiver_low() {}
void deqna::receiver_high() {}
void deqna::reset(const bool) {}
void deqna::show_state(console *const) const {}
bool deqna::test(console *const) { return false; }
void deqna::set_monitor_mode(const deqna::monitor_mode_t mode, console *const cnsl) {
  monitor_mode = mode;
  this->cnsl = cnsl;
}
uint8_t deqna::read_byte(const uint16_t) { return 0; }
uint16_t deqna::read_word(const uint16_t) { return 0; }
void deqna::write_byte(const uint16_t, const uint8_t) {}
void deqna::write_word(const uint16_t, const uint16_t) {}
void get_deqna_mac(uint8_t *const to) {
  if (to) {
    memset(to, 0, 6);
  }
}

tm_11::tm_11(bus *const b) : b(b) {}
tm_11::~tm_11() {}
void tm_11::load(const std::string &) {}
void tm_11::unload() {}
void tm_11::reset(const bool) {}
void tm_11::show_state(console *const) const {}
uint8_t tm_11::read_byte(const uint16_t) { return 0; }
uint16_t tm_11::read_word(const uint16_t) { return 0; }
void tm_11::write_byte(const uint16_t, const uint8_t) {}
void tm_11::write_word(const uint16_t, uint16_t) {}

#endif
