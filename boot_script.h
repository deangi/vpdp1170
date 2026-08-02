#pragma once
#include <stddef.h>
#include <stdint.h>

struct AppConfig;

// Prompt-driven boot answers from [console] boot_script.
// Armed on each PDP reboot; observes KL11 console output bytes and, after a
// short settle delay, injects replies when the current expect substring
// matches (case-insensitive).

void boot_script_arm(const AppConfig& cfg);
void boot_script_disarm();
void boot_script_observe(uint8_t c);
void boot_script_poll();   // call from loop(); fires deferred replies
bool boot_script_active();
