#pragma once
#include <stdint.h>
#include <stddef.h>

// Public surface of the PDP-11/40 CPU core.
//
// Cloned from v8088/cpu8086.h on 2026-05-23. The function NAMES are kept the
// same as the 8088 core so the rest of the project (vpdp1140.ino, ui.cpp,
// etc.) needs only the smallest dispatch change. The implementations behind
// these names will be sam11 (Chloe Lunn) wired up to our PSRAM + I/O-page
// dispatch routing KL11 / RK11 / KW11-L registers; see plan
// .claude/plans/enumerated-skipping-glade.md for milestones.
//
// Memory model: a single 248-KiB block in PSRAM owned by the core. PDP-11/40
// addresses are 18 bits physical (0..0o777777); the top 8 KiB (0o760000..)
// is the I/O page and is dispatched, not stored. sam11 caps usable RAM at
// 0o760000 = 253 952 bytes; we allocate 256 KiB (the next power of two)
// from PSRAM for alignment-friendly access.

#define VPDP_RAM_SIZE   0x40000u    // 256 KiB guest RAM (sam11 18-bit MMU max)

// Lifecycle ---------------------------------------------------------------

// Allocate guest RAM in PSRAM and zero it. Build sam11 dispatch tables.
// Safe to call multiple times - returns true if already initialized.
// (m0 stub: returns true without allocating; sam11 lands in m1.)
bool cpu_init();

// PDP-11 reset: clear R0..R6, set PSW=0, R7 (PC) := bootstrap-ROM entry.
// (m0 stub: no-op; m3 wires it to the bootstrap ROM.)
void cpu_reset();

// Full cold boot: wipe RAM, re-stamp the bootstrap ROM into high memory,
// then cpu_reset(). Use this for the menu's "Reboot PDP-11".
// (m0 stub: no-op.)
void cpu_cold_boot();

// Execute up to `max_cycles` PDP-11 instructions. Returns count actually run;
// stops early if the core halts (HALT opcode, or guest writes to MEMERR with
// no handler). (m0 stub: returns 0 immediately so loop() idles.)
uint32_t cpu_run(uint32_t max_cycles);

// Convenience: stop the CPU loop. (Sets a flag the next cpu_run() reads.)
void cpu_request_halt();

// Front-panel-style monitor control. Pause takes effect between instructions.
// A monitor step executes exactly one instruction and leaves the CPU paused.
void cpu_monitor_pause();
void cpu_monitor_continue();
bool cpu_monitor_paused();
uint32_t cpu_monitor_step();
void cpu_monitor_trace_next(uint32_t count);
uint32_t cpu_monitor_trace_remaining();

// Memory + register accessors --------------------------------------------

uint8_t* cpu_mem();                 // base of the 256 KiB block (PSRAM); NULL until cpu_init()
uint32_t cpu_mem_size();            // == VPDP_RAM_SIZE

// PDP-11 has 8 general registers (R0..R7); R6=SP, R7=PC. PSW is separate.
// idx 0..7 -> R0..R7.
uint16_t cpu_reg16(int idx);
uint16_t cpu_pc();                  // == cpu_reg16(7)
uint16_t cpu_psw();                 // processor status word
uint32_t cpu_inst_count();

// Return the virtual PC and opcode that the next CPU step will fetch.
bool cpu_next_instruction(uint16_t* address, uint16_t* opcode);
bool cpu_disassemble_next(char* buffer, size_t size);

// Examine/deposit an aligned word in 18-bit physical RAM. The I/O page is
// intentionally excluded so monitor commands cannot accidentally operate
// devices or trigger a bus-error trap outside the CPU execution context.
bool cpu_read_physical_word(uint32_t address, uint16_t* value);
bool cpu_write_physical_word(uint32_t address, uint16_t value);

// Manually set PC - used by test rigs and to vector the CPU at a bootstrap
// loaded into RAM. (m0 stub: no-op.)
void cpu_set_pc(uint16_t pc);

// Dump the last `n_entries` PCs from the instruction-trace ring to Serial,
// most-recent first. Cheap diagnostic for tracking down "stuck in a loop"
// states - if PC + R0..R7 don't move much between state-dumps but inst
// count is climbing, this shows which instructions are actually running.
void cpu_dump_trace(int n_entries);

// Enable/disable the per-instruction trace ring. It is very useful for
// panic diagnosis, but it costs a full MMU decode + instruction read +
// register snapshot on every guest instruction, so production boots keep it
// disabled unless [diag] trace=true.
void cpu_set_trace(bool enabled);

// Select which boot ROM cpu_reset() will install before transferring control
// to the boot path. 0 = RL11 (bootrom_rl0, for RL02 packs like XXDP+),
// 1 = RK11 (bootrom_rk0, for RK05 packs like RT-11 v5). Default is RL.
// Call this BEFORE cpu_reset()/cpu_cold_boot() to pick a different boot path.
void cpu_set_boot_kind(int kind);

// m1 acceptance test: write a tiny PDP-11 program (MOV #5, R0; MOV #7, R1;
// ADD R0, R1; BR .-2) directly to PSRAM, run it ~20 cycles, assert
// R0==5 and R1==12. Logs PASS/FAIL on Serial. Returns true on PASS.
// Call between cpu_init() and start_cpu(); start_cpu()'s subsequent
// kd11::reset() reinstalls the bootrom and clobbers the test program.
bool cpu_selftest();
