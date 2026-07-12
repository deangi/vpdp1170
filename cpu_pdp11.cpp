// cpu_pdp11.cpp - PDP-11/40 CPU core wrapper around sam11.
//
// Cloned from v8088/cpu8086.cpp on 2026-05-23. m0 was the no-op stub;
// m1 (this revision) wires up sam11's kd11 (11/40 CPU), kt11 (MMU),
// ms11 (RAM - replaced with our PSRAM-backed version), dd11 (UNIBUS
// dispatcher), and the stubbed device modules (kl11, rk11, kw11,
// lp11, ky11). We do NOT include sam11.cpp because it defines its
// own setup()/loop()/panic() that would collide with the host sketch;
// instead this file mirrors sam11.cpp's loop0() pattern inside
// cpu_run().
//
// PDP-11/23 was the original ask; the project went 1170 -> 1140 on
// 2026-05-23 because sam11's MMU is hard 18-bit / 248 KiB.
// https://pdp-11.org.ru/en/files.pl - images of various products

#include "cpu_pdp11.h"
#include "platform.h"
#include "sam11.h"
#include "sam11_platform.h"
#include "kd11.h"
#include "kt11.h"
#include "ms11.h"
#include "kl11.h"
#include "kw11.h"
#include "kwp.h"
#include "dl11_file.h"
#include "rk11.h"
#include "rl11.h"
#include "rh11.h"
#include "lp11.h"
#include "ky11.h"
#include "dd11.h"
#include "bootrom.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <setjmp.h>

// ---- globals normally defined in sam11.cpp ----
// kd11.cpp already defines `pdp11::intr itab[ITABN]`.
jmp_buf trapbuf;

// User-mode label tables (kd11/kt11/etc. reference these in debug paths).
const char* users_str[4]  = { "kernel", "illegal", "illegal", "user" };
const char  users_char[4] = { 'K', 'X', 'X', 'U' };

// ---- ring buffer of last N instructions for post-HALT diagnosis ----
// Each cpu_run iteration captures (PC, opcode, R0..R7, PS) before the
// instruction executes. On panic() we dump the most recent entries so
// the user can identify which sam11 instruction tripped the HALT.
struct TraceEntry {
  uint16_t pc;
  uint16_t instr;
  uint16_t r[8];
  uint16_t ps;
};
#define TRACE_RING_SIZE 512
static TraceEntry s_trace_ring[TRACE_RING_SIZE];
static uint32_t   s_trace_idx = 0;  // next write slot
static bool       s_trace_enable = false;

// ---- ring of last N traps (synchronous: bus error, reserved instr, EMT,
// TRAP, IOT, BPT, ...). Records the source PC and the new PC the vector
// points at, so we can see WHICH vector fired and WHERE V4B's handler
// for that vector lives. Dumped after the instruction trace on panic().
struct TrapEntry {
  uint16_t vec;
  uint16_t pc_in;   // PC when the trap fired (R7 at setjmp return)
  uint16_t pc_out;  // PC after trapat() loaded the vector
  uint16_t sp;      // SP when the trap fired
};
#define TRAP_RING_SIZE 32
static TrapEntry s_trap_ring[TRAP_RING_SIZE];
static uint32_t  s_trap_idx = 0;
static constexpr bool DISK_IRQ_TRACE = false;
static int       s_disk_irq_trace_left = 32;

// sam11 stubs (referenced only in PRINTSIMLINES / DEBUG paths, all
// compile-time false in our build, but the symbols still need to exist).
// Note: printstate() and disasm() are NOT defined here - they live in
// disasm.cpp which is part of the vendored sam11 set.
void trap(uint16_t /*num*/) { /* deferred to sam11's own trap path */ }

// panic() is called by sam11 on conditions it considers unrecoverable
// (HALT instruction, RESET in user mode, invalid current/previous
// user-mode bits, etc.). We log the CPU state, set a flag, and longjmp
// out so the host loop can recover and continue running (instead of
// deadlocking core 1).
volatile bool g_panicked = false;
volatile bool g_serial_silenced = false;
static volatile bool s_panic_dumped = false;  // first panic-dump latch
void panic() {
  // Idempotent: only the FIRST panic emits the dump + trace ring. Any
  // subsequent panic() call (recursive bus error, second trap during
  // trap, second cpu_run on a still-broken state) just longjmps out
  // silently so the captured trace stays clean on the monitor.
  if (s_panic_dumped) {
    g_panicked = true;
    longjmp(trapbuf, 0);
  }
  s_panic_dumped = true;

  // Print everything BEFORE we set g_serial_silenced. The LOGE / Serial.printf
  // below would otherwise be gated by it.
  LOGE("PDP-11 panic: halted  PC=0%o  R0=0%o R1=0%o R2=0%o R3=0%o R4=0%o R5=0%o SP=0%o  PS=0%o",
       (unsigned)kd11::curPC,
       (unsigned)kd11::R[0], (unsigned)kd11::R[1], (unsigned)kd11::R[2],
       (unsigned)kd11::R[3], (unsigned)kd11::R[4], (unsigned)kd11::R[5],
       (unsigned)kd11::R[6], (unsigned)kd11::PS);
  // Memory window around curPC to spot the trapping instruction.
  uint32_t pc = kd11::curPC;
  Serial.printf("[vpdp1170]   mem near PC:");
  for (int o = -4; o <= 4; o++) {
    uint32_t a = (pc + o*2) & 0xFFFF;
    Serial.printf(" %s%06o", o == 0 ? "[" : " ", (unsigned)dd11::read16(a));
    if (o == 0) Serial.printf("]");
  }
  Serial.printf("\r\n");

  // Dump the last TRACE_RING_SIZE instructions leading up to the HALT.
  // s_trace_idx points at the next write slot, so the oldest valid
  // entry is at (s_trace_idx - TRACE_RING_SIZE) mod TRACE_RING_SIZE.
  //
  // ESP32-S3 USB-CDC has a per-write timeout (default ~100ms); on
  // bursty output the host drains slower than we write, write() returns
  // a short count, and bytes get silently dropped. Two defenses:
  //   1) bump the TX timeout very high so write() *waits* for room
  //      instead of giving up
  //   2) flush() + brief sleep after every line so the host always has
  //      a quiet moment to drain before we hit it again
  Serial.setTxTimeoutMs(5000);
  Serial.printf("[vpdp1170]   --- last %d instructions before HALT ---\r\n",
                TRACE_RING_SIZE);
  Serial.flush();
  for (int n = TRACE_RING_SIZE; n > 0; n--) {
    uint32_t i = (s_trace_idx + TRACE_RING_SIZE - n) % TRACE_RING_SIZE;
    TraceEntry& e = s_trace_ring[i];
    if (e.pc == 0 && e.instr == 0) continue;  // empty slot before fill
    Serial.printf("  PC=%06o ins=%06o R0=%06o R1=%06o R2=%06o R3=%06o R4=%06o R5=%06o SP=%06o PS=%06o\r\n",
                  (unsigned)e.pc, (unsigned)e.instr,
                  (unsigned)e.r[0], (unsigned)e.r[1], (unsigned)e.r[2],
                  (unsigned)e.r[3], (unsigned)e.r[4], (unsigned)e.r[5],
                  (unsigned)e.r[6], (unsigned)e.ps);
    Serial.flush();
    delay(2);
  }
  // Dump the recent trap history. Each entry is "vector V from PC X
  // diverted to PC Y" — useful for spotting which V4B handler we ended
  // up in (e.g. INTBUS=04, INTINVAL=10, KW11=100, RK=220, RL=160).
  Serial.printf("[vpdp1170]   --- last %d traps ---\r\n", TRAP_RING_SIZE);
  Serial.flush();
  for (int n = TRAP_RING_SIZE; n > 0; n--) {
    uint32_t i = (s_trap_idx + TRAP_RING_SIZE - n) % TRAP_RING_SIZE;
    TrapEntry& e = s_trap_ring[i];
    if (e.vec == 0 && e.pc_in == 0) continue;
    Serial.printf("  vec=%03o  from PC=%06o (SP=%06o) -> new PC=%06o\r\n",
                  (unsigned)e.vec, (unsigned)e.pc_in,
                  (unsigned)e.sp,  (unsigned)e.pc_out);
    Serial.flush();
    delay(2);
  }
  Serial.flush();

  Serial.printf("[vpdp1170]   --- end panic dump, serial silenced ---\r\n");

  g_panicked = true;

  // Silence all further USB-Serial output so the trace ring above is the
  // last thing on the monitor and easy to capture. Reset in cpu_reset().
  Serial.flush();
  g_serial_silenced = true;

  // Bail out of step() via the trap path so cpu_run can return.
  longjmp(trapbuf, 0);
}

// ---- our PSRAM-backed guest memory ----
static uint8_t*       s_mem = nullptr;
static uint32_t       s_inst_count = 0;
static volatile bool  s_halt_requested = false;
static volatile bool  s_monitor_paused = false;
static volatile uint32_t s_monitor_trace_left = 0;
static bool           s_sam11_inited = false;
// 0 = RL (bootrom_rl0), 1 = RK (bootrom_rk0), 2 = RP (bootrom_rp0).
// The host sketch sets this from cfg.boot_kind before calling cpu_reset().
static int            s_boot_kind = 0;

void cpu_set_boot_kind(int kind) {
  s_boot_kind = (kind == 1 || kind == 2) ? kind : 0;
}


bool cpu_init() {
  if (s_mem) return true;
  s_mem = (uint8_t*)heap_caps_aligned_alloc(
      32, VPDP_RAM_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_mem) {
    LOGE("cpu_init: PSRAM alloc failed (%u bytes)", (unsigned)VPDP_RAM_SIZE);
    return false;
  }
  memset(s_mem, 0, VPDP_RAM_SIZE);
  s_inst_count     = 0;
  s_halt_requested = false;
  s_monitor_paused = false;
  s_monitor_trace_left = 0;
  ms11::begin();
  // First call to cpu_reset() will pull in the bootrom and reset the CPU.
  s_sam11_inited = false;
  LOG("cpu_init: %u bytes guest RAM allocated in PSRAM (sam11 KD11/KT11 wired)",
      (unsigned)VPDP_RAM_SIZE);
  return true;
}

void cpu_reset() {
  if (!s_mem) return;
  // kd11::reset() does the heavy lifting: zeros the GPRs/PS/MMU, calls
  // kw11::reset() + ms11::clear() + kl11::reset() + rk11::reset(), writes
  // sam11's bootrom_rk0 boot block to BOOT_START in RAM, sets R7 to
  // BOOT_START. We add ky11::reset() (the front panel) since sam11.cpp
  // does that outside kd11::reset(), and rl11::reset() because the host
  // boots from RL02 not RK05.
  ky11::reset();
  kd11::reset();
  rl11::reset();
  rk11::reset();
  rh11::reset();
  lp11::reset();
  kwp::reset();
  dl11_file::reset();

  // Install the chosen boot ROM at BOOT_START (02000 octal). kd11::reset()
  // installs bootrom_rk0 by default; we overwrite that region with RL, RK,
  // or RP bootstrap as selected by s_boot_kind.
  if (s_boot_kind == 1) {
    const uint32_t rk_words = sizeof(bootrom_rk0) / sizeof(uint16_t);
    LOG("cpu_reset: loading RK0 boot ROM (%u words) at PC = 0%o",
        (unsigned)rk_words, (unsigned)BOOT_START);
    for (uint32_t i = 0; i < rk_words; i++) {
      dd11::write16(BOOT_START + (i * 2), bootrom_rk0[i]);
    }
  } else if (s_boot_kind == 2) {
    const uint32_t rp_words = sizeof(bootrom_rp0) / sizeof(uint16_t);
    LOG("cpu_reset: loading RP0 boot ROM (%u words) at PC = 0%o",
        (unsigned)rp_words, (unsigned)BOOT_START);
    for (uint32_t i = 0; i < rp_words; i++) {
      dd11::write16(BOOT_START + (i * 2), bootrom_rp0[i]);
    }
  } else {
    const uint32_t rl_words = sizeof(bootrom_rl0) / sizeof(uint16_t);
    LOG("cpu_reset: loading RL0 boot ROM (%u words) at PC = 0%o",
        (unsigned)rl_words, (unsigned)BOOT_START);
    for (uint32_t i = 0; i < rl_words; i++) {
      dd11::write16(BOOT_START + (i * 2), bootrom_rl0[i]);
    }
  }

  // ----- Banner program at 01000 -----
  // A tiny native PDP-11 program that prints the current scaffold banner
  // to the KL11 console then jumps into the RL0 boot ROM at 02000. Lets
  // us verify the KL11 -> TFT/Telnet/Serial pipe independent of any
  // disk-boot path. Code + message live below the bootrom, well below
  // any stack the bootrom will set up (SP = 02000).
  //
  //   01000  MOV   #MSG, R0       ; 012700 + 01100
  //   01004  MOVB  (R0)+, R1
  //   01006  BEQ   +6              ; if char==0 -> JMP @#02000
  //   01010  TSTB  @#TPS           ; 105737 + 0177564  (TWO words!)
  //   01014  BPL   -3              ; back to TSTB (skip BOTH its words)
  //   01016  MOVB  R1, @#TPB       ; 110137 + 0177566
  //   01022  BR    -8              ; next char
  //   01024  JMP   @#02000         ; 000137 + 002000
  //   01100  "vpdp1170: booting 11/40 scaffold...\r\n\0"
  static const uint16_t banner_prog[] = {
    0012700, 0001100,   // MOV  #01100, R0
    0112001,            // MOVB (R0)+, R1
    0001406,            // BEQ  +6 -> JMP at 01024
    0105737, 0177564,   // TSTB @#TPS  (2-word instr)
    0100375,            // BPL  -3 -> back to 01010 TSTB
    0110137, 0177566,   // MOVB R1, @#TPB  (2-word instr)
    0000770,            // BR   -8 -> back to 01004 MOVB(R0)+
    0000137, 0002000,   // JMP  @#02000
  };
  const uint32_t banner_words = sizeof(banner_prog) / sizeof(uint16_t);
  for (uint32_t i = 0; i < banner_words; i++) {
    dd11::write16(0001000 + (i * 2), banner_prog[i]);
  }
  const char* msg = (s_boot_kind == 1)
                  ? "vpdp1170: booting 11/40 scaffold from RK0...\r\n"
                  : (s_boot_kind == 2)
                    ? "vpdp1170: booting 11/40 scaffold from RP0...\r\n"
                    : "vpdp1170: booting 11/40 scaffold from DL0...\r\n";
  uint8_t* bytes = (uint8_t*)s_mem;
  uint32_t mi = 0;
  while (msg[mi]) { bytes[0001100 + mi] = (uint8_t)msg[mi]; mi++; }
  bytes[0001100 + mi] = 0;
  LOG("cpu_reset: banner installed at 01000, msg at 01100 (%u chars)", (unsigned)mi);

  // Start the CPU at the banner program (01000); it prints + JMP @#02000.
  kd11::R[7]  = 0001000;
  kd11::curPC = 0001000;
  LOG("cpu_reset: ready, R7 = 0%o (banner -> bootrom @02000)",
      (unsigned)kd11::R[7]);

  g_panicked = false;
  g_serial_silenced = false;
  s_panic_dumped = false;

  // Clear the instruction-trace ring so the post-HALT dump shows only
  // entries from this run, not stale ones from the previous boot.
  for (int i = 0; i < TRACE_RING_SIZE; i++) {
    s_trace_ring[i].pc = 0;
    s_trace_ring[i].instr = 0;
  }
  s_trace_idx = 0;

  // Same for the trap ring.
  for (int i = 0; i < TRAP_RING_SIZE; i++) {
    s_trap_ring[i].vec = 0;
    s_trap_ring[i].pc_in = 0;
    s_trap_ring[i].pc_out = 0;
    s_trap_ring[i].sp = 0;
  }
  s_trap_idx = 0;
  s_disk_irq_trace_left = 32;

  s_sam11_inited = true;
  s_halt_requested = false;
  s_monitor_paused = false;
  s_monitor_trace_left = 0;
}

void cpu_cold_boot() {
  if (s_mem) memset(s_mem, 0, VPDP_RAM_SIZE);
  cpu_reset();
}

// Step the CPU up to max_cycles times. Mirrors sam11.cpp's loop0() pattern:
// pending-interrupt check, kd11::step(), kw11::tick(), kl11::poll(). Traps
// are caught by the setjmp/longjmp pair (kd11/dd11 longjmp to trapbuf with
// the trap vector, and we route it back through kd11::trapat()).
uint32_t cpu_run(uint32_t max_cycles) {
  if (!s_sam11_inited || g_panicked || s_monitor_paused) {
    yield();
    return 0;
  }

  uint32_t executed = 0;
  // Catch panics (longjmp from panic()) BEFORE we'd otherwise route them
  // through kd11::trapat() with a bogus odd vector and recurse forever.
  uint16_t vec = setjmp(trapbuf);
  if (g_panicked) {
    return executed;
  }
  if (vec) {
    // Record the trap BEFORE trapat() rewrites R7/SP so we know where it
    // fired from. After trapat() runs we re-read R7 to know where the
    // vector sent us. Helps diagnose "which handler did V4B install for X?"
    uint16_t pc_in = kd11::R[7];
    uint16_t sp_in = kd11::R[6];
    kd11::trapat(vec);
    TrapEntry& te = s_trap_ring[s_trap_idx];
    te.vec    = vec;
    te.pc_in  = pc_in;
    te.sp     = sp_in;
    te.pc_out = kd11::R[7];
    s_trap_idx = (s_trap_idx + 1) % TRAP_RING_SIZE;
    return executed;  // re-enter cpu_run() to install a fresh trapbuf
  }

  while (executed < max_cycles && !s_halt_requested && !g_panicked) {
    // Pending interrupt? handleinterrupt() loads the new PC/PSW from the
    // vector and returns - it may longjmp out for nested-trap cases, in
    // which case the setjmp at the top of this function will catch it.
    // Do NOT return here, or we never get to actually run an instruction
    // when the line clock or other periodic IRQ is pending most of the
    // time (MIPS would read 0).
    // PDP-11 interrupt requests are accepted only when their BR level is
    // strictly greater than the current PSW priority. Accepting an equal
    // level makes a BR4 KL11 appear to software as a higher-priority device.
    if (itab[0].vec && (itab[0].pri > ((kd11::PS >> 5) & 7))) {
      if (DISK_IRQ_TRACE && s_disk_irq_trace_left > 0 &&
          (itab[0].vec == INTRK || itab[0].vec == INTRL || itab[0].vec == INTRP)) {
        LOG("CPU IRQ deliver vec=%03o BR%u PC=%06o PS=%06o",
            (unsigned)itab[0].vec, (unsigned)itab[0].pri,
            (unsigned)kd11::R[7], (unsigned)kd11::PS);
        s_disk_irq_trace_left--;
      }
      kd11::handleinterrupt();
    }
    // Record this instruction in the trace ring BEFORE step() runs,
    // so on panic() we can see exactly what was about to execute.
    if (s_trace_enable) {
      TraceEntry& e = s_trace_ring[s_trace_idx];
      e.pc    = kd11::R[7];
      e.instr = dd11::read16(kt11::decode_instr(kd11::R[7], false, kd11::curuser));
      for (int i = 0; i < 8; i++) e.r[i] = (uint16_t)kd11::R[i];
      e.ps    = kd11::PS;
      s_trace_idx = (s_trace_idx + 1) % TRACE_RING_SIZE;
    }
    if (s_monitor_trace_left > 0) {
      uint16_t pc = (uint16_t)kd11::R[7];
      uint32_t physical = kt11::decode_instr(pc, false, kd11::curuser);
      uint16_t instruction = dd11::read16(physical);
      char disassembly[128];
      disasm_format(physical, pc, disassembly, sizeof(disassembly));
      LOG("trace: PC=%06o ins=%06o R0=%06o R1=%06o R2=%06o R3=%06o R4=%06o R5=%06o SP=%06o PS=%06o %s",
          (unsigned)pc, (unsigned)instruction,
          (unsigned)((uint16_t)kd11::R[0]),
          (unsigned)((uint16_t)kd11::R[1]),
          (unsigned)((uint16_t)kd11::R[2]),
          (unsigned)((uint16_t)kd11::R[3]),
          (unsigned)((uint16_t)kd11::R[4]),
          (unsigned)((uint16_t)kd11::R[5]),
          (unsigned)((uint16_t)kd11::R[6]),
          (unsigned)kd11::PS,
          disassembly);
      s_monitor_trace_left--;
    }
    kd11::step();
    kw11::tick();
    kwp::tick();    // KW11-P programmable clock countdown
    dl11_file::poll();
    rk11::tick();   // drives the deferred RK-done IRQ countdown
    rl11::tick();   // drives the deferred RL-done IRQ countdown
    rh11::tick();   // drives the deferred RH/RP-done IRQ countdown
    lp11::poll();
    kl11::poll();
    executed++;
    s_inst_count++;
  }
  return executed;
}

void cpu_request_halt() { s_halt_requested = true; }
void cpu_set_trace(bool enabled) { s_trace_enable = enabled; }

void cpu_monitor_pause() {
  s_monitor_paused = true;
}

void cpu_monitor_continue() {
  s_monitor_paused = false;
}

bool cpu_monitor_paused() {
  return s_monitor_paused;
}

uint32_t cpu_monitor_step() {
  if (!s_sam11_inited || g_panicked) return 0;
  s_monitor_paused = false;
  uint32_t executed = cpu_run(1);
  s_monitor_paused = true;
  return executed;
}

void cpu_monitor_trace_next(uint32_t count) {
  s_monitor_trace_left = count;
}

uint32_t cpu_monitor_trace_remaining() {
  return s_monitor_trace_left;
}

uint8_t* cpu_mem()       { return s_mem; }
uint32_t cpu_mem_size()  { return VPDP_RAM_SIZE; }

uint16_t cpu_reg16(int idx) {
  if (idx < 0 || idx > 7) return 0;
  return (uint16_t)kd11::R[idx];
}
uint16_t cpu_pc()         { return kd11::curPC; }
uint16_t cpu_psw()        { return kd11::PS; }
uint32_t cpu_inst_count() { return s_inst_count; }

bool cpu_next_instruction(uint16_t* address, uint16_t* opcode) {
  if (!s_sam11_inited || !address || !opcode) return false;
  *address = (uint16_t)kd11::R[7];
  *opcode = dd11::read16(
      kt11::decode_instr(*address, false, kd11::curuser));
  return true;
}

bool cpu_disassemble_next(char* buffer, size_t size) {
  if (!s_sam11_inited || !buffer || size == 0) return false;
  uint16_t address = (uint16_t)kd11::R[7];
  uint32_t physical = kt11::decode_instr(address, false, kd11::curuser);
  return disasm_format(physical, address, buffer, size);
}

bool cpu_read_physical_word(uint32_t address, uint16_t* value) {
  if (!s_mem || !value || (address & 1) || address >= 0760000u) return false;
  *value = (uint16_t)s_mem[address]
         | ((uint16_t)s_mem[address + 1] << 8);
  return true;
}

bool cpu_write_physical_word(uint32_t address, uint16_t value) {
  if (!s_mem || (address & 1) || address >= 0760000u) return false;
  s_mem[address] = (uint8_t)(value & 0xff);
  s_mem[address + 1] = (uint8_t)(value >> 8);
  return true;
}

void cpu_set_pc(uint16_t pc) { kd11::R[7] = pc; kd11::curPC = pc; }

void cpu_dump_trace(int n_entries) {
  if (n_entries <= 0) return;
  if (n_entries > TRACE_RING_SIZE) n_entries = TRACE_RING_SIZE;
  Serial.printf("[vpdp1170]   trace (last %d):\r\n", n_entries);
  for (int n = n_entries; n > 0; n--) {
    uint32_t i = (s_trace_idx + TRACE_RING_SIZE - n) % TRACE_RING_SIZE;
    TraceEntry& e = s_trace_ring[i];
    if (e.pc == 0 && e.instr == 0) continue;
    Serial.printf("  PC=%06o ins=%06o  R0=%06o R1=%06o R2=%06o R3=%06o R4=%06o R5=%06o SP=%06o PS=%06o\r\n",
                  (unsigned)e.pc, (unsigned)e.instr,
                  (unsigned)e.r[0], (unsigned)e.r[1], (unsigned)e.r[2],
                  (unsigned)e.r[3], (unsigned)e.r[4], (unsigned)e.r[5],
                  (unsigned)e.r[6], (unsigned)e.ps);
  }
}

// ---- m1 self-test --------------------------------------------------------
//
// Writes the following PDP-11 program at octal address 02000:
//   02000: 012700  MOV #5, R0      \ two-word: opcode + immediate
//   02002: 000005
//   02004: 012701  MOV #7, R1      \ two-word: opcode + immediate
//   02006: 000007
//   02010: 060001  ADD R0, R1      ; R1 := R0 + R1 = 5 + 7 = 12 (014 octal)
//   02012: 000777  BR .-2          ; spin in place
// After ~20 cycles we expect R0==5, R1==014. The BR-to-self lets us verify
// without relying on HALT (sam11's HALT handler triggers special behavior).

bool cpu_selftest() {
  if (!s_mem) {
    LOGE("cpu_selftest: PSRAM not allocated");
    return false;
  }
  // Initialize sam11 state. kd11::reset() also installs the RK0 bootrom at
  // BOOT_START=02000; we overwrite that region with our test program.
  kd11::reset();
  ky11::reset();

  uint16_t* w = (uint16_t*)s_mem;
  uint32_t addr = 0002000;
  w[addr >> 1] = 0012700; addr += 2;
  w[addr >> 1] = 0000005; addr += 2;
  w[addr >> 1] = 0012701; addr += 2;
  w[addr >> 1] = 0000007; addr += 2;
  w[addr >> 1] = 0060001; addr += 2;
  w[addr >> 1] = 0000777; // BR .-2 (spin)

  kd11::R[7]   = 0002000;
  kd11::curPC  = 0002000;
  kd11::PS     = 0;
  s_sam11_inited = true;

  uint32_t pre_inst = s_inst_count;
  uint32_t executed = cpu_run(20);
  uint32_t r0 = (uint32_t)kd11::R[0];
  uint32_t r1 = (uint32_t)kd11::R[1];
  uint32_t r7 = (uint32_t)kd11::R[7];

  bool pass = (r0 == 5) && (r1 == 014); // 014 octal = 12 decimal

  LOG("cpu_selftest: R0=%o R1=%o R7=%o ran=%u total=%u -> %s",
      (unsigned)r0, (unsigned)r1, (unsigned)r7,
      (unsigned)executed, (unsigned)(s_inst_count - pre_inst),
      pass ? "PASS" : "FAIL");

  // Reset s_inst_count so the MIPS readout in the status bar starts at 0
  // when the real boot begins; the test instructions otherwise show up as
  // a one-time spike in the rate.
  s_inst_count = 0;
  return pass;
}
