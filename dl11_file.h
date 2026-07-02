#pragma once

#include <stddef.h>
#include <stdint.h>

namespace dl11_file {

void set_enabled(bool enabled);
bool enabled();

void reset();
void poll();
void host_poll();

uint16_t read16(uint32_t address);
void write16(uint32_t address, uint16_t value);

bool open_input(const char* path, int eof_byte, bool notify);
void close_input();
bool input_connected();
bool input_at_eof();
const char* input_path();

bool open_output(const char* path, bool append);
void close_output();
bool output_connected();
const char* output_path();
bool output_append_mode();

// Direct access to the same connected streams used by TT1. Reads consume
// buffered TT1 input first. Writes drain pending TT1 output first and flush
// the SD file before returning.
size_t read_stream(uint8_t* data, size_t max_bytes);
bool write_stream(const uint8_t* data, size_t bytes);

void disconnect_all();

}  // namespace dl11_file
