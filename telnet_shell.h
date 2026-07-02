#pragma once

#include <stddef.h>
#include <stdint.h>

void telnet_shell_init();
void telnet_shell_enter();
void telnet_shell_disconnect();
bool telnet_shell_active();

// Called by the Telnet task on core 0. Returns true when the character should
// be echoed by the Telnet server. CR queues the completed command.
bool telnet_shell_input(uint8_t c);
bool telnet_shell_backspace();

// Called by the emulator loop on core 1. All SD and emulator mutations happen
// here, never in the core-0 Telnet task.
void telnet_shell_poll();

// Core-0 consumer for shell responses produced on core 1.
size_t telnet_shell_output_peek(const uint8_t** data);
void telnet_shell_output_consume(size_t bytes);
