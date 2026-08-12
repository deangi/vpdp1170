#pragma once
#include <stddef.h>
#include <stdint.h>

// Second Telnet listener for DZ11 active line. No management shell — raw
// 8-bit path with minimal Telnet IAC negotiation. Port/enabled come from
// /wificonfig.ini [dz11].

void        telnet_dz_begin(uint16_t port, bool enabled);
void        telnet_dz_poll();
void        telnet_dz_write(uint8_t c);
bool        telnet_dz_in_pop(uint8_t* out);
bool        telnet_dz_in_available();
bool        telnet_dz_connected();
bool        telnet_dz_listening();
const char* telnet_dz_client_ip();
uint16_t    telnet_dz_port();
bool        telnet_dz_enabled();
