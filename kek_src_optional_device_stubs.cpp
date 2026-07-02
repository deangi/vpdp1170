#include "config.h"

#if VPDP1170_USE_KEK_CORE && VPDP1170_BUILD_KEK_ADAPTER

// Temporary link stubs for kek peripherals that are referenced by bus.cpp but
// are not part of the first ESP32 bring-up slice. Replace these with real
// kek_src_<device>.cpp wrappers one device at a time as each device is ported.

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
void tty::notify_rx() {
  int tks = (PDP11TTY_TKS - PDP11TTY_BASE) / 2;
  int tkb = (PDP11TTY_TKB - PDP11TTY_BASE) / 2;
  if ((registers[tks] & 0200) == 0) {
    uint8_t ch = 0;
    if (!pop_kek_console_input(&ch)) return;
    registers[tkb] = ch & 0177;
    registers[tks] |= 0200;
  }
  if (b && b->getCpu() &&
      (registers[tks] & 0100)) {
    b->getCpu()->queue_interrupt(4, 060);
  }
}
void tty::reset(const bool hard) {
  if (hard) {
    memset(registers, 0, sizeof(registers));
  }
  registers[(PDP11TTY_TPS - PDP11TTY_BASE) / 2] = 0200;
}
uint8_t tty::read_byte(const uint16_t addr) {
  uint16_t v = read_word(addr & ~1);
  return (addr & 1) ? (uint8_t)(v >> 8) : (uint8_t)v;
}
uint16_t tty::read_word(const uint16_t addr) {
  int reg = (addr - PDP11TTY_BASE) / 2;
  if (reg < 0 || reg >= 4) return 0;

  if (addr == PDP11TTY_TKS) {
    notify_rx();
    return registers[reg];
  }

  if (addr == PDP11TTY_TKB) {
    uint16_t value = registers[reg] & 0177;
    registers[(PDP11TTY_TKS - PDP11TTY_BASE) / 2] &= ~0200;
    notify_rx();
    return value;
  }

  if (addr == PDP11TTY_TPS) {
    registers[reg] |= 0200;
  }

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
    registers[reg] = (registers[reg] & 0200) | (v & 0100);
    notify_rx();
    return;
  }

  if (addr == PDP11TTY_TPS) {
    registers[reg] = (registers[reg] & 0200) | (v & 0100);
    if (b && b->getCpu() && (registers[reg] & 0200) && (registers[reg] & 0100)) {
      b->getCpu()->queue_interrupt(4, 064);
    }
    return;
  }

  if (addr == PDP11TTY_TPB) {
    uint8_t out = v & 0177;
    console_feed(out);
    telnet_write(out);
    if (!g_serial_silenced) {
      Serial.write(out);
    }
    registers[(PDP11TTY_TPS - PDP11TTY_BASE) / 2] |= 0200;
    if (b && b->getCpu() &&
        (registers[(PDP11TTY_TPS - PDP11TTY_BASE) / 2] & 0100)) {
      b->getCpu()->queue_interrupt(4, 064);
    }
  }

  registers[reg] = v;
}
void tty::operator()() {}

rl02::rl02(bus *const b, abool *const disk_read_activity, abool *const disk_write_activity)
  : b(b), registers{}, xfer_buffer{}, mpr{}, disk_read_activity(disk_read_activity), disk_write_activity(disk_write_activity) {}
rl02::~rl02() {}
void rl02::begin() {}
void rl02::reset(const bool) {}
void rl02::show_state(console *const) const {}
uint8_t rl02::read_byte(const uint16_t) { return 0; }
uint16_t rl02::read_word(const uint16_t) { return 0; }
void rl02::write_byte(const uint16_t, const uint8_t) {}
void rl02::write_word(const uint16_t, const uint16_t) {}

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
