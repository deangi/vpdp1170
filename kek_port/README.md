# vpdp1170 kek port staging

This directory is a staging area for the Arduino-side PDP-11/70 adapter.
The sketch-root `pdp_core_kek.cpp` includes `pdp_core_kek.cpp.disabled`
only when both `VPDP1170_USE_KEK_CORE` and `VPDP1170_BUILD_KEK_ADAPTER` are
set to 1, because Arduino does not compile sketch subdirectories by itself.

The current working Arduino build still uses:

```cpp
#define VPDP1170_USE_KEK_CORE 0
#define VPDP1170_BUILD_KEK_ADAPTER 0
```

The intended first real kek bring-up is:

1. Build the minimal kek object graph used by the Visual Studio harness:
   `bus`, `memory`, `mmu`, and `cpu`.
2. Allocate 512 kek memory pages, which is `512 * 8192 = 4 MB`.
3. Reset the CPU, load the MOV/MOV/ADD/BR self-test at octal `001000`,
   step the CPU, and prove `R0=5`, `R1=14`.
4. Expose monitor-friendly state: registers, PSW, next opcode,
   disassembly, physical examine/deposit, single-step, and trace lines.
5. Continue adding the selected kek upstream source files and include shims
   until the adapter builds in Arduino.

The sketch root also contains `kek_src_optional_device_stubs.cpp`. Those
stubs are temporary and only compile when the two kek adapter switches are on.
They satisfy references from kek `bus.cpp` for optional devices that are not
part of the first CPU/MMU self-test slice. Replace each stub with the real
upstream device wrapper as that device is deliberately ported.

TTY is the first exception to the inert-stub rule: its methods now implement
the PDP-visible console status/data registers and bridge bytes to the existing
vpdp TFT, Telnet, and USB-Serial queues. It still lives in the same file until
the console phase is mature enough to split into a dedicated wrapper.

Do not move `.cpp.disabled` files into the sketch root. Keep the root wrapper
as the single compile gate so the default scaffold build remains unaffected.
