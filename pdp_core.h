#pragma once
#include <stdint.h>
#include <stddef.h>

// vpdp1170 CPU-engine boundary.
//
// The scaffold currently uses the inherited vpdp1140/sam11-compatible public
// CPU surface. Host code should include this file, not a concrete engine
// header, so the later kek PDP-11/70 import has one obvious swap point.
//
// Planned replacements behind this facade:
//   - kek CPU / MMU / bus / memory
//   - 4 MB PSRAM-backed physical memory
//   - 22-bit I/O page and Unibus-map-aware DMA

#include "config.h"

#if !VPDP1170_USE_KEK_CORE
#include "cpu_pdp11.h"
#elif VPDP1170_BUILD_KEK_ADAPTER
#include "kek_port/pdp_core_kek.h"
#endif

namespace pdp_core {

#if VPDP1170_USE_KEK_CORE
#if VPDP1170_BUILD_KEK_ADAPTER
static constexpr const char* kEngineName = "kek PDP-11/70 adapter";
#else
static constexpr const char* kEngineName = "kek PDP-11/70 not wired";
#endif
static constexpr bool kIsKek = true;
#else
static constexpr const char* kEngineName = "sam11 PDP-11/40 scaffold";
static constexpr bool kIsKek = false;
#endif

inline const char* engine_name() { return kEngineName; }
inline bool is_kek_engine() { return kIsKek; }

#if VPDP1170_USE_KEK_CORE

#if VPDP1170_BUILD_KEK_ADAPTER

// Kek-backed adapter. This deliberately does not forward into the inherited
// 11/40 core, so flipping VPDP1170_USE_KEK_CORE to 1 cannot accidentally run
// the wrong PDP-visible machine.
inline bool init() { return pdp_core_kek::init(); }
inline void set_target_memory_kw(uint32_t kw) { pdp_core_kek::set_target_memory_kw(kw); }
inline uint32_t target_memory_bytes() { return pdp_core_kek::target_memory_bytes(); }
inline void reset() { pdp_core_kek::reset(); }
inline void cold_boot() { pdp_core_kek::cold_boot(); }
inline uint32_t run(uint32_t max_cycles) { return pdp_core_kek::run(max_cycles); }
inline bool selftest() { return pdp_core_kek::selftest(); }

inline uint8_t* memory() { return pdp_core_kek::memory(); }
inline uint32_t memory_size() { return pdp_core_kek::memory_size(); }
inline uint16_t reg16(int idx) { return pdp_core_kek::reg16(idx); }
inline uint16_t pc() { return pdp_core_kek::pc(); }
inline uint16_t psw() { return pdp_core_kek::psw(); }
inline bool set_reg16(int idx, uint16_t value) {
  return pdp_core_kek::set_reg16(idx, value);
}
inline bool set_psw(uint16_t value) { return pdp_core_kek::set_psw(value); }
inline uint32_t instruction_count() { return pdp_core_kek::instruction_count(); }
inline bool next_instruction(uint16_t* address, uint16_t* opcode) {
  return pdp_core_kek::next_instruction(address, opcode);
}
inline bool disassemble_next(char* buffer, size_t size) {
  return pdp_core_kek::disassemble_next(buffer, size);
}
inline bool read_physical_word(uint32_t address, uint16_t* value) {
  return pdp_core_kek::read_physical_word(address, value);
}
inline bool write_physical_word(uint32_t address, uint16_t value) {
  return pdp_core_kek::write_physical_word(address, value);
}
inline bool read_mmu_word(uint16_t address, uint16_t* value) {
  return pdp_core_kek::read_mmu_word(address, value);
}
inline bool read_rp06_word(uint16_t address, uint16_t* value) {
  return pdp_core_kek::read_rp06_word(address, value);
}
inline bool get_rp06_deferred(bool* active, int* delay, int* cs1_polls,
                              int* wc_polls) {
  return pdp_core_kek::get_rp06_deferred(active, delay, cs1_polls, wc_polls);
}
inline bool get_mmu_summary(uint16_t* mmr0, uint16_t* mmr1, uint16_t* mmr2,
                            uint16_t* mmr3, uint16_t* cpuerr, uint16_t* pir,
                            uint32_t* io_base) {
  return pdp_core_kek::get_mmu_summary(mmr0, mmr1, mmr2, mmr3, cpuerr, pir,
                                       io_base);
}
inline bool get_mmu_page(int run_mode, bool data_space, int page,
                         uint16_t* par, uint16_t* pdr,
                         uint32_t* physical_base) {
  return pdp_core_kek::get_mmu_page(run_mode, data_space, page, par, pdr,
                                    physical_base);
}
inline bool get_unibus_map_entry(int entry, uint32_t* base) {
  return pdp_core_kek::get_unibus_map_entry(entry, base);
}
inline bool get_interrupt_summary(uint16_t* psw, bool* any_pending,
                                  uint8_t counts[8],
                                  uint16_t first_vectors[8]) {
  return pdp_core_kek::get_interrupt_summary(psw, any_pending, counts,
                                             first_vectors);
}
inline bool get_kw11l_summary(uint16_t* csr, uint32_t* us_since_tick,
                              bool* irq_queued) {
  return pdp_core_kek::get_kw11l_summary(csr, us_since_tick, irq_queued);
}

inline void set_boot_kind(int kind) { pdp_core_kek::set_boot_kind(kind); }
inline void set_trace(bool enabled) { pdp_core_kek::set_trace(enabled); }
inline void set_dl_trace(uint32_t count) { pdp_core_kek::set_dl_trace(count); }
inline uint32_t dl_trace_remaining() { return pdp_core_kek::dl_trace_remaining(); }
inline void set_rp_trace(uint32_t count) { pdp_core_kek::set_rp_trace(count); }
inline uint32_t rp_trace_remaining() { return pdp_core_kek::rp_trace_remaining(); }
inline void monitor_pause() { pdp_core_kek::monitor_pause(); }
inline void monitor_continue() { pdp_core_kek::monitor_continue(); }
inline bool monitor_paused() { return pdp_core_kek::monitor_paused(); }
inline uint32_t monitor_step() { return pdp_core_kek::monitor_step(); }
inline void monitor_trace_next(uint32_t count) { pdp_core_kek::monitor_trace_next(count); }
inline uint32_t monitor_trace_remaining() { return pdp_core_kek::monitor_trace_remaining(); }
inline void monitor_dump_history() { pdp_core_kek::monitor_dump_history(); }

#else

// Kek selected but the Arduino source dependency set has not been imported
// yet. Keep this as a deliberate "not wired" path until
// VPDP1170_BUILD_KEK_ADAPTER is enabled.
inline bool init() { return false; }
inline void set_target_memory_kw(uint32_t) {}
inline uint32_t target_memory_bytes() { return VPDP1170_TARGET_RAM_BYTES; }
inline void reset() {}
inline void cold_boot() {}
inline uint32_t run(uint32_t) { return 0; }
inline bool selftest() { return false; }

inline uint8_t* memory() { return nullptr; }
inline uint32_t memory_size() { return 0; }
inline uint16_t reg16(int) { return 0; }
inline uint16_t pc() { return 0; }
inline uint16_t psw() { return 0; }
inline bool set_reg16(int, uint16_t) { return false; }
inline bool set_psw(uint16_t) { return false; }
inline uint32_t instruction_count() { return 0; }
inline bool next_instruction(uint16_t*, uint16_t*) { return false; }
inline bool disassemble_next(char* buffer, size_t size) {
  if (buffer && size) {
    const char* msg = "kek core not wired";
    size_t i = 0;
    while (i + 1 < size && msg[i]) {
      buffer[i] = msg[i];
      i++;
    }
    buffer[i] = 0;
  }
  return false;
}
inline bool read_physical_word(uint32_t, uint16_t*) { return false; }
inline bool write_physical_word(uint32_t, uint16_t) { return false; }
inline bool read_mmu_word(uint16_t, uint16_t*) { return false; }
inline bool read_rp06_word(uint16_t, uint16_t*) { return false; }
inline bool get_rp06_deferred(bool*, int*, int*, int*) { return false; }
inline bool get_mmu_summary(uint16_t*, uint16_t*, uint16_t*, uint16_t*,
                            uint16_t*, uint16_t*, uint32_t*) { return false; }
inline bool get_mmu_page(int, bool, int, uint16_t*, uint16_t*,
                         uint32_t*) { return false; }
inline bool get_unibus_map_entry(int, uint32_t*) { return false; }
inline bool get_interrupt_summary(uint16_t*, bool*, uint8_t[8],
                                  uint16_t[8]) {
  return false;
}
inline bool get_kw11l_summary(uint16_t*, uint32_t*, bool*) { return false; }

inline void set_boot_kind(int) {}
inline void set_trace(bool) {}
inline void set_dl_trace(uint32_t) {}
inline uint32_t dl_trace_remaining() { return 0; }
inline void set_rp_trace(uint32_t) {}
inline uint32_t rp_trace_remaining() { return 0; }
inline void monitor_pause() {}
inline void monitor_continue() {}
inline bool monitor_paused() { return true; }
inline uint32_t monitor_step() { return 0; }
inline void monitor_trace_next(uint32_t) {}
inline uint32_t monitor_trace_remaining() { return 0; }
inline void monitor_dump_history() {}

#endif

#else

// Lifecycle.
inline bool init() { return cpu_init(); }
inline void set_target_memory_kw(uint32_t) {}
inline uint32_t target_memory_bytes() { return VPDP1170_TARGET_RAM_BYTES; }
inline void reset() { cpu_reset(); }
inline void cold_boot() { cpu_cold_boot(); }
inline uint32_t run(uint32_t max_cycles) { return cpu_run(max_cycles); }
inline bool selftest() { return cpu_selftest(); }

// Guest state.
inline uint8_t* memory() { return cpu_mem(); }
inline uint32_t memory_size() { return cpu_mem_size(); }
inline uint16_t reg16(int idx) { return cpu_reg16(idx); }
inline uint16_t pc() { return cpu_pc(); }
inline uint16_t psw() { return cpu_psw(); }
inline bool set_reg16(int, uint16_t) { return false; }
inline bool set_psw(uint16_t) { return false; }
inline uint32_t instruction_count() { return cpu_inst_count(); }
inline bool next_instruction(uint16_t* address, uint16_t* opcode) {
  return cpu_next_instruction(address, opcode);
}
inline bool disassemble_next(char* buffer, size_t size) {
  return cpu_disassemble_next(buffer, size);
}
inline bool read_physical_word(uint32_t address, uint16_t* value) {
  return cpu_read_physical_word(address, value);
}
inline bool write_physical_word(uint32_t address, uint16_t value) {
  return cpu_write_physical_word(address, value);
}
inline bool read_mmu_word(uint16_t address, uint16_t* value) {
  return cpu_read_physical_word(address, value);
}
inline bool read_rp06_word(uint16_t, uint16_t*) { return false; }
inline bool get_rp06_deferred(bool*, int*, int*, int*) { return false; }
inline bool get_mmu_summary(uint16_t*, uint16_t*, uint16_t*, uint16_t*,
                            uint16_t*, uint16_t*, uint32_t*) { return false; }
inline bool get_mmu_page(int, bool, int, uint16_t*, uint16_t*,
                         uint32_t*) { return false; }
inline bool get_unibus_map_entry(int, uint32_t*) { return false; }
inline bool get_interrupt_summary(uint16_t*, bool*, uint8_t[8],
                                  uint16_t[8]) {
  return false;
}
inline bool get_kw11l_summary(uint16_t*, uint32_t*, bool*) { return false; }

inline void set_boot_kind(int kind) { cpu_set_boot_kind(kind); }
inline void set_trace(bool enabled) { cpu_set_trace(enabled); }
inline void set_dl_trace(uint32_t) {}
inline uint32_t dl_trace_remaining() { return 0; }
inline void set_rp_trace(uint32_t) {}
inline uint32_t rp_trace_remaining() { return 0; }
inline void monitor_pause() { cpu_monitor_pause(); }
inline void monitor_continue() { cpu_monitor_continue(); }
inline bool monitor_paused() { return cpu_monitor_paused(); }
inline uint32_t monitor_step() { return cpu_monitor_step(); }
inline void monitor_trace_next(uint32_t count) { cpu_monitor_trace_next(count); }
inline uint32_t monitor_trace_remaining() { return cpu_monitor_trace_remaining(); }
inline void monitor_dump_history() { cpu_dump_trace(512); }

#endif

}  // namespace pdp_core
