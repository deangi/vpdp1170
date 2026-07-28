#pragma once
#include <stdint.h>
#include <stddef.h>

// vpdp1170 CPU-engine boundary — always the kek PDP-11/70 adapter.
// Host code includes this file, not kek_port headers directly.
// The inherited sam11/11/40 scaffold lives under legacy_sam11/ for reference.

#include "config.h"
#include "kek_port/pdp_core_kek.h"

namespace pdp_core {

static constexpr const char* kEngineName = "kek PDP-11/70 adapter";
static constexpr bool kIsKek = true;

inline const char* engine_name() { return kEngineName; }
inline bool is_kek_engine() { return kIsKek; }

inline bool init() { return pdp_core_kek::init(); }
inline void set_target_memory_kw(uint32_t kw) { pdp_core_kek::set_target_memory_kw(kw); }
inline uint32_t target_memory_bytes() { return pdp_core_kek::target_memory_bytes(); }
inline void reset() { pdp_core_kek::reset(); }
inline void cold_boot() { pdp_core_kek::cold_boot(); }
inline uint32_t run(uint32_t max_cycles) { return pdp_core_kek::run(max_cycles); }
inline bool selftest() { return pdp_core_kek::selftest(); }
inline bool benchmark() { return pdp_core_kek::benchmark(); }

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
inline bool set_rp06_operator_stop(bool stopped) {
  return pdp_core_kek::set_rp06_operator_stop(stopped);
}
inline bool get_rp06_operator_stop(bool* stopped) {
  return pdp_core_kek::get_rp06_operator_stop(stopped);
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
inline bool get_console_switches(uint16_t* value) {
  return pdp_core_kek::get_console_switches(value);
}
inline bool set_console_switches(uint16_t value) {
  return pdp_core_kek::set_console_switches(value);
}
inline bool set_console_switch(int bit, bool state) {
  return pdp_core_kek::set_console_switch(bit, state);
}
inline bool get_console_lights(uint16_t* address, uint32_t* physical_address,
                               uint16_t* data, bool* data_valid,
                               uint16_t* leds) {
  return pdp_core_kek::get_console_lights(address, physical_address, data,
                                          data_valid, leds);
}

inline void set_boot_kind(int kind) { pdp_core_kek::set_boot_kind(kind); }
inline void set_trace(bool enabled) { pdp_core_kek::set_trace(enabled); }
inline void set_dl_trace(uint32_t count) { pdp_core_kek::set_dl_trace(count); }
inline uint32_t dl_trace_remaining() { return pdp_core_kek::dl_trace_remaining(); }
inline void set_du_trace(uint32_t count) { pdp_core_kek::set_du_trace(count); }
inline uint32_t du_trace_remaining() { return pdp_core_kek::du_trace_remaining(); }
inline void set_rp_trace(uint32_t count) { pdp_core_kek::set_rp_trace(count); }
inline uint32_t rp_trace_remaining() { return pdp_core_kek::rp_trace_remaining(); }
inline void monitor_pause() { pdp_core_kek::monitor_pause(); }
inline void monitor_continue() { pdp_core_kek::monitor_continue(); }
inline bool monitor_paused() { return pdp_core_kek::monitor_paused(); }
inline uint32_t monitor_step() { return pdp_core_kek::monitor_step(); }
inline void monitor_trace_next(uint32_t count) { pdp_core_kek::monitor_trace_next(count); }
inline uint32_t monitor_trace_remaining() { return pdp_core_kek::monitor_trace_remaining(); }
inline void monitor_dump_history() { pdp_core_kek::monitor_dump_history(); }
inline void monitor_break_clear() { pdp_core_kek::monitor_break_clear(); }
inline bool monitor_break_set_pc(uint16_t pc) {
  return pdp_core_kek::monitor_break_set_pc(pc);
}
inline bool monitor_break_active() { return pdp_core_kek::monitor_break_active(); }
inline uint16_t monitor_break_pc() { return pdp_core_kek::monitor_break_pc(); }

}  // namespace pdp_core
