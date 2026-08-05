#pragma once

#include <stddef.h>
#include <stdint.h>

// L3 NAT helper for DEUNA: ARP + gateway ICMP locally; outbound IPv4
// UDP/TCP/ICMP rewritten onto the WiFi STA address (userspace NAPT).
namespace eth_nat {

static constexpr size_t MAX_FRAME = 1514;
static constexpr size_t MIN_FRAME = 60;

void reset();
void set_guest_mac(const uint8_t mac[6]);
void set_gateway_mac(const uint8_t mac[6]);
void set_addresses(uint32_t guest_ip_host, uint32_t guest_mask_host,
                   uint32_t gateway_ip_host);
void set_sta_ip(uint32_t sta_ip_host);

// Process an Ethernet frame transmitted by the guest. May enqueue reply
// frames for deuna RX and/or forward via WiFi NAPT.
void on_guest_tx(const uint8_t* frame, size_t len);

// Expire idle NAT mappings; safe on the PDP poll path (no socket I/O).
void tick();

// Run WiFi-side NAT I/O (ICMP socket send/recv). Call from net_task only.
void host_poll();

// Pop next frame queued for the guest RX ring. Returns false if empty.
bool pop_rx(uint8_t* out, size_t* out_len, size_t out_cap);

size_t rx_pending();

}  // namespace eth_nat
