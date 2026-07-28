#pragma once
#include <stddef.h>
#include <stdint.h>

// Host console bridge for the kek TTY path. Guest CSRs live in kek_src_tty.
namespace kl11 {

void reset();
void poll();
uint16_t read16(uint32_t a);
void write16(uint32_t a, uint16_t v);

// Minimum host-side ms between successive TKB loads. 0 disables the gate.
extern uint32_t serial_in_delay_ms;

void set_console_trace(uint32_t count);
uint32_t console_trace_remaining();
void charge_console_trace(const char* direction, uint8_t value);

// Drain queued KL11->USB-Serial bytes (called from loop() on core 1).
void drain_serial_out();

// Non-blocking enqueue of a guest console output byte (USB-Serial sink).
void queue_serial_out(uint8_t out);

// Parse guest console output for the ESC ] VPDP ; ... ETX control channel
// and forward printable bytes to TFT / Telnet / USB-Serial.
void handle_guest_output(uint8_t out);

// Queue a framed emulator-control response into the guest receive stream.
bool queue_control_reply(const char* payload);

// Queue unframed bytes into the guest receive stream (INASCII/INHEX).
bool queue_input_bytes(const uint8_t* data, size_t bytes);

// Pop the next control-reply / injected input byte (before USB/Telnet).
bool pop_priority_input(uint8_t* out);

}  // namespace kl11
