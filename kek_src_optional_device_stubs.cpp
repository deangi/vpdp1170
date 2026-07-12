#include "config.h"

#if VPDP1170_USE_KEK_CORE && VPDP1170_BUILD_KEK_ADAPTER

// Link stubs for kek peripherals not yet ported. Console KL11 lives in
// kek_src_tty.cpp (Option 3) — do not implement tty:: here.

#include <string.h>

#include "_upstream_kek/dc11.h"
#include "_upstream_kek/deqna.h"
#include "_upstream_kek/dz11.h"
#include "_upstream_kek/tm-11.h"

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
