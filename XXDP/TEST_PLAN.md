# XXDP PDP-11/70 Emulator Validation Plan

## Goal

Use XXDP V2.2 diagnostics to isolate latent faults in PDP-11 instruction,
trap/interrupt, MMU, clock, and disk-controller emulation before returning to
the RSX-11M-PLUS V4.6 bugcheck.

## Rules and evidence

- Work from `pdpconfig-xxdp22.ini` and keep one unchanged baseline config.
- Save the full console log for every run. Name it by phase and diagnostic.
- Record diagnostic filename/version, switches, hardware table answers, first
  failing test, PC, expected/actual values, and whether the failure repeats.
- Run one pass first. Increase to 10 passes only after a clean single pass.
- Change only one emulator variable between comparison runs.
- Never run format, write, compatibility, performance, or exerciser tests on
  the boot image or any irreplaceable disk image.
- Create dedicated scratch RK05, RL02, and RP06 images before device testing.

## Phase 0 - Inventory and baseline

1. Boot XXDP V2.2 from DL0 and capture `DIR` output.
2. Build an availability table for the diagnostic names below, including the
   exact revision suffix present on the disk.
3. Record the effective emulator configuration and memory size.
4. Run a monitor-only smoke test: boot, `DIR`, load one known diagnostic,
   return to the monitor, and reboot.
5. Preserve the original `xxdp22.dsk`; make a working copy if XXDP writes to it.

Gate: do not interpret `file not found` as a hardware/emulator failure. Acquire
the missing diagnostic binary or use the closest applicable diagnostic listed
by `DIR`.

## Phase 1 - Basic CPU instructions

Run in this order, using the exact revision found on the disk:

1. `FKAAC0` - PDP-11/34 basic instruction test (known to be present).
2. `FKACA1` - EIS instruction test, if present.
3. `EKBA` - PDP-11/70 CPU test 1, preferred when present.
4. `EKBB` - PDP-11/70 CPU test 2, preferred when present.
5. `EQKC` - PDP-11/70 instruction exerciser, after deterministic tests pass.

Coverage focus: addressing modes, byte/word flag behavior, branches, JSR/RTS,
MUL/DIV/ASH/ASHC, SOB/XOR, register sets, PSW operations, and stack behavior.

Failure procedure: rerun only the failing test with CPU trace bounded around
the failure/HALT. Compare the failing instruction against the diagnostic
listing before changing emulation.

## Phase 2 - Traps and interrupts

1. `FKABD0` - traps test (known project regression target).
2. The trap portion of `EKBA`/`EKBB`, if available.
3. Verify odd-address, reserved-instruction, BPT, IOT, EMT, TRAP, trace-bit,
   stack-limit/red-zone, and privilege traps.
4. Verify saved PC semantics separately for instruction fetch faults and
   operand faults.
5. Verify interrupt priority ordering, masking at equal/lower IPL, deferred
   delivery after SPL/RTI/RTT, previous-mode PS bits, and kernel stack pushes.

Gate: all deterministic CPU/trap tests must pass before MMU or device failures
are treated as device-model defects.

## Phase 3 - MMU, memory, and UNIBUS map

1. `EKBE` - PDP-11/70 memory-management diagnostic.
2. `EKBF` - PDP-11/70 UNIBUS-map diagnostic.
3. `EMJA` / `EMKA` - PDP-11/70 memory tests, where available.
4. Use `ZKDK` only as supplemental coverage; it targets KDJ11 behavior and is
   not authoritative for every 11/70 distinction.

Run MMU tests with 124 KW first, then 256 KW and 504 KW/512 KW configured.
Check kernel/supervisor/user I/D spaces, PAR/PDR access control and length
direction, MMR0 abort latching/freeze, MMR1 register deltas, MMR2 fault PC,
MMR3 22-bit and I/D enables, previous-mode instructions, and I/O-page mapping.

For DMA tests, verify 18-bit UNIBUS addresses, BAE bits, UNIBUS map translation,
wraparound, odd addresses, NXM behavior, and transfers crossing 8 KB pages.

## Phase 4 - Clocks

### KW11-L

Run `ZKWA` (or `XKWA` under DEC/X11 if that is what the disk contains).
Verify CSR DONE/monitor bit, IE gating, vector 0100 at BR6, acknowledgement,
no duplicate queued request, frequency, WAIT wakeup, and IPL masking.

### KW11-P

Run `ZKWB` or `XKWB` with `kwp_enabled=true`. Repeat with it false to verify
that an absent/stub device produces no interrupts. Test GO, IE, DONE, ERR,
count direction, repeat/one-shot modes, rates, vector 0104 at BR6, and CSR
read/write side effects.

Do not run both clock diagnostics concurrently during initial isolation.

## Phase 5 - Disk controllers and DMA

### RL11 / RL02

- Controller-first: `ZRLG`, then `ZRLH` (or `XRLA`).
- Drive tests only on a scratch RL02: `ZRLI`, `ZRLJ` if present.
- Performance/compatibility tests (`ZRLK`, `ZRLL`) last.
- Verify GET STATUS, SEEK, READ HEADER, READ/WRITE, word count, BA/BAE,
  cylinder/head/sector geometry, DONE/ERR, and vector 0160 at BR5.

### RK11 / RK05

- Controller-first: `ZRKJ`, then `ZRKK` (or `XRKA`).
- Dynamic/performance tests (`ZRKL`, `ZRKH`) only on a scratch RK05.
- Verify RKDS/RKER/RKCS/RKWC/RKBA/RKDA semantics, seek/reset, geometry,
  DMA boundaries, DONE/ERR, and vector 0220 at BR5.

### RH11/RH70 with RP04/5/6 model

- Prefer diskless/controller tests first: `ZRJG`, `ZRJH`, `ZRJI`, `ZRJJ`, or
  DEC/X11 `XRMD`, according to what is actually present.
- Run `ZRJA` read/write only on a scratch RP image.
- Do not run `ZRJB` formatter or `ZRJC` alignment on preserved media.
- Verify CS1/CS2, DS, ER1, AS, WC, BA/BAE, DA/DC, drive type, attention,
  controller clear, DMA through the UNIBUS map, and vector 0254 at BR5.

## Phase 6 - Cross-subsystem stress

After isolated tests pass:

1. Run a memory exerciser while KW11-L interrupts are active.
2. Run RL/RK/RP read-only activity with clock interrupts active.
3. Exercise simultaneous pending BR4/BR5/BR6 sources.
4. Repeat at 124 KW and 504 KW mapped memory configurations.
5. Run 10 passes, then an extended soak only after deterministic success.

## Decision tree

- Basic instruction failure: fix CPU opcode/addressing/PSW behavior first.
- Trap test failure with CPU tests clean: inspect saved PC/PS, stack switching,
  red-zone behavior, and RTI/RTT.
- MMU failure: isolate translation from abort bookkeeping before device tests.
- Controller register test failure: fix CSR/register semantics before DMA.
- DMA-only failure: inspect BAE/UNIBUS map/page-crossing and word-count rules.
- Clock-only failure: inspect CSR gating and level-request acknowledgement.
- All XXDP phases pass but RSX fails: return to scheduler/task-state tracing;
  the remaining defect is likely an interaction or an uncovered 11/70 detail.

## Results table

For each run record: date, firmware commit, config, diagnostic/revision, test
number, passes, result, PC, expected, actual, last trap/interrupt, and log path.

## Documentation in this folder

- `AC-F348E-MC_XXDP+_Users_Manual_Rev_E_Apr81.pdf`
- `MAINDEC-11-DZQXA-I-D_XXDP_User_Manual_Jul76.pdf`
- `PDP11_DiagnosticHandbook_1988.pdf`
- `EL-ENDIA-11_PDP-11_Diagnostic_Design_Guide_Jan83.pdf`
- `Turnbull_XXDP_Feb93.pdf`
- `AH-FG66P-MC_pdp11DiagIdx_Jun92.txt`

Source archive: https://www.bitsavers.org/pdf/dec/pdp11/xxdp/
