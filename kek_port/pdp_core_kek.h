#pragma once
#include <stdint.h>
#include <stddef.h>

namespace pdp_core_kek {

static constexpr uint32_t kPageBytes = 8192;
static constexpr uint32_t kIoPageKW = 8;
static constexpr uint32_t kFullMemoryPages = 512;
static constexpr uint32_t kFullMemoryBytes = kFullMemoryPages * kPageBytes;

bool init();
void set_target_memory_kw(uint32_t kw);
uint32_t target_memory_bytes();
void reset();
void cold_boot();
uint32_t run(uint32_t max_cycles);
bool selftest();

uint8_t* memory();
uint32_t memory_size();
uint16_t reg16(int idx);
uint16_t pc();
uint16_t psw();
bool set_reg16(int idx, uint16_t value);
bool set_psw(uint16_t value);
uint32_t instruction_count();

bool next_instruction(uint16_t* address, uint16_t* opcode);
bool disassemble_next(char* buffer, size_t size);
bool read_physical_word(uint32_t address, uint16_t* value);
bool write_physical_word(uint32_t address, uint16_t value);
bool read_mmu_word(uint16_t address, uint16_t* value);
bool get_mmu_summary(uint16_t* mmr0, uint16_t* mmr1, uint16_t* mmr2,
                     uint16_t* mmr3, uint16_t* cpuerr, uint16_t* pir,
                     uint32_t* io_base);
bool get_mmu_page(int run_mode, bool data_space, int page, uint16_t* par,
                  uint16_t* pdr, uint32_t* physical_base);
bool get_unibus_map_entry(int entry, uint32_t* base);
bool get_interrupt_summary(uint16_t* psw, bool* any_pending,
                           uint8_t counts[8], uint16_t first_vectors[8]);

void set_boot_kind(int kind);
void set_trace(bool enabled);
void set_dl_trace(uint32_t count);
uint32_t dl_trace_remaining();
void monitor_pause();
void monitor_continue();
bool monitor_paused();
uint32_t monitor_step();
void monitor_trace_next(uint32_t count);
uint32_t monitor_trace_remaining();
void monitor_dump_history();

}  // namespace pdp_core_kek
