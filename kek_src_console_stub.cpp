#include "config.h"

// Minimal console method needed by kek log.cpp during CPU/MMU bring-up.
// No kek console instance is wired yet; the real console bridge will replace
// this when PDP-side TTY is connected to vpdp TFT/Telnet/USB streams.

#include "_upstream_kek/console.h"

void console::put_string(const std::string &) {
}
