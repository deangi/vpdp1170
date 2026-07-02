#pragma once
#include <stdint.h>
#include <stddef.h>

namespace pdp_core_kek {

static constexpr uint32_t kPageBytes = 8192;
static constexpr uint32_t kFullMemoryPages = 512;
static constexpr uint32_t kFullMemoryBytes = kFullMemoryPages * kPageBytes;

bool init();
void reset();
void cold_boot();
uint32_t run(uint32_t max_cycles);
bool selftest();

uint8_t* memory();
uint32_t memory_size();
uint16_t reg16(int idx);
uint16_t pc();
uint16_t psw();
uint32_t instruction_count();

bool next_instruction(uint16_t* address, uint16_t* opcode);
bool disassemble_next(char* buffer, size_t size);
bool read_physical_word(uint32_t address, uint16_t* value);
bool write_physical_word(uint32_t address, uint16_t value);

void set_boot_kind(int kind);
void set_trace(bool enabled);
void monitor_pause();
void monitor_continue();
bool monitor_paused();
uint32_t monitor_step();
void monitor_trace_next(uint32_t count);
uint32_t monitor_trace_remaining();

}  // namespace pdp_core_kek
