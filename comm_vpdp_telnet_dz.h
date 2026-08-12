#pragma once

#include <string>
#include "_upstream_kek/comm.h"
#include "telnet_dz.h"

// Bridges DZ11 active line (via kek comm_io) to the host DZ Telnet FIFOs.
class comm_vpdp_telnet_dz : public comm {
public:
  bool begin() override { return true; }
  bool need_dealloc() override { return true; }

  std::string get_identifier() const override {
    return "vpdp-telnet-dz";
  }

  bool is_connected() override {
    return telnet_dz_connected();
  }

  bool has_data() override {
    return telnet_dz_in_available();
  }

  uint8_t get_byte() override {
    uint8_t c = 0;
    if (!telnet_dz_in_pop(&c))
      return 0;
    return c;
  }

  void send_data(const uint8_t *const in, const size_t n) override {
    if (!in) return;
    for (size_t i = 0; i < n; i++)
      telnet_dz_write(in[i]);
  }
};
