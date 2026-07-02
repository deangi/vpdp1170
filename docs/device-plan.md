# vpdp1170 Device Source Plan

`vpdp1170` starts as a clone of `vpdp1140` so the ESP32 host services stay
working while the PDP-visible machine is replaced with a PDP-11/70 model.
The working rule is:

> PDP-visible CPU, bus, memory, MMU, DMA, and devices should come from `kek`
> unless there is a specific reason to keep a `vpdp1140` implementation.
> ESP32-visible host services should remain from `vpdp1140`.

## Source Snapshots

- `vpdp1140` baseline: copied into this sketch tree on 2026-07-01.
- `kek` reference: copied into `_upstream_kek` from
  <https://github.com/folkertvanheusden/kek>.

## Device Matrix

| Area | Preferred source | vpdp1140 status | kek status | Action |
|---|---|---|---|---|
| CPU | kek | KD11 / sam11 11/40-derived core | `cpu.cpp/.h` PDP-11/70 engine | Replace with kek behind a host adapter. |
| MMU | kek | KT11-like 18-bit model | `mmu.cpp/.h` | Replace; must support full 22-bit 11/70 behavior. |
| Memory | kek + vpdp host allocator | 248 KiB PSRAM block | `memory.cpp/.h` | Allocate 4 MB in ESP32-S3 PSRAM and expose via kek bus/memory callbacks. |
| Bus / I/O page | kek | `dd11.cpp/.h` 18-bit I/O dispatch | `bus.cpp/.h` | Use kek bus model; keep vpdp I/O trace ideas as diagnostics. |
| Console TTY | kek device, vpdp host I/O | `kl11.cpp/.h` routed to TFT/Telnet/USB | `tty.cpp/.h`, console adapters | Use kek PDP-side TTY behavior, connect it to vpdp console/telnet/TFT. |
| KW11-L | kek | `kw11.cpp/.h` | `kw11-l.cpp/.h` | Prefer kek; compare against vpdp fixes and diagnostics. |
| KW11-P | vpdp reference until kek equivalent found | `kwp.cpp/.h` | not yet identified in snapshot | Keep vpdp implementation as candidate/reference if no kek device exists. |
| RK05 / RK11 | kek | `rk11.cpp/.h` | `rk05.cpp/.h` | Prefer kek; preserve vpdp implementation as regression reference. |
| RL01/RL02 | kek | `rl11.cpp/.h` | `rl02.cpp/.h` | Prefer kek; verify RL01/RL02 size handling and boot behavior. |
| RH11/RP04/RP05/RP06 | kek | `rh11.cpp/.h` testing-mode support | `rp06.cpp/.h` | Prefer kek; make this a first-class 11/70 storage path. |
| TM11 tape | kek | not present in active vpdp1140 | `tm-11.cpp/.h` | New capability; useful for UNIX install/diagnostics later. |
| DC11 | kek | not present in active vpdp1140 | `dc11.cpp/.h` | New capability; evaluate after basic console boot. |
| DZ11 | kek | not present in active vpdp1140 | `dz11.cpp/.h` | New capability; useful for multi-user UNIX after core boot. |
| DEQNA | kek reference only | not present in active vpdp1140 | `deqna.cpp/.h` | Defer; network device emulation is later-phase work. |
| Disk backends | hybrid | SD_MMC disk image code | file/NBD/ESP32 disk backend code | Use vpdp SD mounting UI; adapt backend calls to kek devices. |
| Telnet/FTP/UI/menu/status/shell/monitor | vpdp1140 | mature ESP32 host services | not applicable | Keep vpdp host code. |
| VPDP escape commands / SD file bridge | vpdp1140 | implemented host service | not applicable | Keep vpdp host code. |

## Implementation Phases

1. **Scaffold baseline**
   - Keep current `vpdp1170` build working with inherited `vpdp1140` core.
   - Rename visible app identity.
   - Keep `_upstream_kek` as reference source.

2. **Core adapter**
   - Define a neutral PDP core adapter for reset/run/trace/registers.
   - Host code includes `pdp_core.h`; this currently forwards to
     `cpu_pdp11.h` and later becomes the `kek` swap point.
   - The Arduino sketch reports the active engine at boot and in System Info.
   - `config.h` contains the bring-up switch:
     `VPDP1170_USE_KEK_CORE=0` means inherited PDP-11/40 scaffold,
     `VPDP1170_USE_KEK_CORE=1` selects the kek path rather than accidentally
     running the 11/40 scaffold. `VPDP1170_BUILD_KEK_ADAPTER=0` keeps that path
     in a deliberate "not wired" state until the kek source dependency set is
     ready for Arduino.
   - Keep the host UI, telnet, FTP, shell, and monitor independent of the CPU engine.

3. **kek CPU/MMU/memory bring-up**
   - Create a minimal Arduino `kek` object graph: `memory`, `mmu`, `bus`, `cpu`.
   - Staging code lives in `kek_port/`; the sketch-root `pdp_core_kek.cpp`
     includes it only when both kek switches are enabled.
   - Allocate 4 MB guest memory in PSRAM.
   - Prove reset and instruction stepping with a RAM-resident MOV/MOV/ADD/BR
     self-test like the Visual Studio harness.
   - Use temporary optional-device stubs only to satisfy `bus.cpp` links during
     the first CPU/MMU self-test; replace them with real device wrappers as
     each device is ported.
   - Only after that, connect console-visible output.

4. **Console and clock**
   - Replace PDP-side KL/TTY behavior with kek TTY logic.
   - First Arduino bridge stage: attach a kek `tty` to the kek bus and route
     its input/output through the existing vpdp TFT/Telnet/USB host streams.
   - Add KW11-L from kek, preserving vpdp trace controls.

5. **Block devices and DMA**
   - Bring up RK05, RL02, then RP06.
   - Confirm DMA uses 22-bit physical addressing and the Unibus map.
   - Keep vpdp disk picker/mount UI but route operations into kek devices.

6. **New kek devices**
   - Add TM11 tape after disks are stable.
   - Add DC11/DZ11 for multi-user UNIX.
   - Defer DEQNA/network devices until core OS targets are stable.

7. **Validation**
   - Start with CPU/MMU diagnostics and XXDP.
   - Then UNIX V7 and 2.11BSD.
   - Then RSX/RSTS targets that need 11/70 memory behavior.

## Open Decisions

- Whether to compile selected kek files directly or import them into a thinner
  `pdp1170_core` adapter layer.
- Whether `pdpconfig.ini` remains the active config name or becomes
  `pdp1170config.ini`.
- Whether the first bootable 11/70 target should be RK/RL-based UNIX V7 or
  RP06-based 2.11BSD.
