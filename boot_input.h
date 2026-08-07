#pragma once
#include <stddef.h>
#include <stdint.h>

struct AppConfig;

// Timed typeahead from [console] boot_input, including optional <<seconds>>
// delay markers. Armed on each PDP reboot; poll from the main loop.
// Disarm on emulator reset aborts any in-flight delay so keys cannot leak
// into the next boot.

void boot_input_arm(const AppConfig& cfg);
void boot_input_disarm();
void boot_input_poll();
bool boot_input_active();
