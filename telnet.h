#pragma once
#include <stdint.h>

// Single-client Telnet server. The guest console (the BIOS ANSI/PUTCHAR
// stream) is sent raw to the client; client keystrokes are queued to the
// guest keyboard. A telnet client is itself an ANSI terminal, so no
// translation of the output stream is needed.

void        telnet_begin(uint16_t port, bool enabled);
void        telnet_poll();              // call every loop: accept + RX + flush TX
void        telnet_write(uint8_t c);    // queue one console-output byte

// Host diagnostics (reset banners, HALT dumps): queued even while the
// management shell is active, and flushed alongside shell/console output.
void        telnet_diag_write(uint8_t c);
void        telnet_diag_text(const char* text);

// Pop the next byte that arrived from the connected telnet client.
// Returns true if a byte was dequeued. kl11::poll() reads this.
bool        telnet_in_pop(uint8_t* out);

bool        telnet_connected();
bool        telnet_listening();
const char* telnet_client_ip();          // "" when no client
uint16_t    telnet_port();
bool        telnet_enabled();
bool        telnet_shell_connected();     // client is in management shell
