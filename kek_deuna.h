#pragma once

#include <stdint.h>

class bus;

// Unibus DEUNA (DELUA-compatible CSR window) with eth_nat ARP/ICMP/NAPT.
namespace kek_deuna {

static constexpr uint16_t BASE_ADDR = 0174510;
static constexpr uint16_t END_ADDR  = 0174520;  // 4 PCSRs, 8 bytes
static constexpr uint16_t VECTOR    = 0120;
static constexpr uint8_t  BR_LEVEL  = 5;

void set_enabled(bool on);
bool enabled();
void set_bus(bus* b);
void set_mac(const uint8_t mac[6]);
void get_mac(uint8_t mac[6]);
void set_network(uint32_t guest_ip, uint32_t guest_mask, uint32_t gateway_ip);

bool     contains(uint16_t addr);
void     reset();
void     tick();
bool     take_interrupt();
uint16_t read_word(uint16_t addr);
uint8_t  read_byte(uint16_t addr);
void     write_word(uint16_t addr, uint16_t value);
void     write_byte(uint16_t addr, uint8_t value);

}  // namespace kek_deuna
