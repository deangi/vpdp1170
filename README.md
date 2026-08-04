# vpdp1170 — a DEC PDP-11/70 emulator for the ESP32-S3 with a touch screen display.

> Development status: **V2.5** (2026-08-03). Guest OS boot matrix below
> (RSTS/E V7 through Ready; Unix V6/V7; 2.11BSD on RL/RP; RSX-11M / M+;
> RT-11; XXDP; DOSBATCH). See `status.txt` for the full working notes.
> The ESP32 host side is inherited from `vpdp1140`: TFT console, touch menu,
> Telnet, FTP, SD card configuration, monitor/shell, and disk image management.
> The inherited 11/40-derived scaffold remains in the tree for reference and
> fallback while the `kek` device set is brought across in phases.

A **Freenove ESP32-S3 2.8" Display** board turned into a tiny DEC
PDP-11/70 that boots guest OSes from SD-card disk images. The console
appears on the onboard TFT, on Telnet, and on USB-Serial — all three live
simultaneously.

For full operating instructions, SD card setup, configuration-file reference,
and menu documentation, see the [PDP 11/70 Emulator User Guide](docs/user-manual.md).
For the 11/70 engine replacement plan and device source decisions, see
[vpdp1170 Device Source Plan](docs/device-plan.md).

## Source tree layout

Arduino IDE compiles every `.cpp` / `.ino` in the **sketch root** only (it does
not recurse into subfolders). Upstream kek sources and the adapter live under
subdirectories and are pulled in by thin sketch-root wrappers.

| Path | Role |
|------|------|
| `vpdp1170.ino`, `config.h`, `board_*.h`, `gfx.h` | Sketch entry, board select, display abstraction |
| `console.*`, `ui.*`, `touch.*`, `telnet*`, `ftp.*`, `disk.*`, `appconfig.*` | ESP32 host services (TFT, network, SD mounts) |
| `kek_src_*.cpp`, `pdp_core_kek.cpp`, `kek_kwp.*` | Sketch-root wrappers that `#include` kek sources |
| `kek_port/pdp_core_kek_impl.cpp`, `kek_port/pdp_core_kek.h` | kek PDP-11/70 adapter (included, not auto-compiled) |
| `_upstream_kek/` | Vendored kek engine + devices (subset used via wrappers) |
| `legacy_sam11/` | Inherited vpdp1140/sam11 scaffold (reference only; not compiled) |
| `docs/`, `documentation/` | User docs, screenshots, manuals, hardware assets |
| `PdpSdCard/` | Sample SD-card configs (copy to a live TF card) |
| `tools/` | Font generator, diagnostics toolkit, Windows kek harness |
| `kl11-implementation/`, `CrowPanelBringup/` | Reference / local bring-up (not part of shipped firmware) |

![vpdp1170 running RT-11 V5](docs/images/rt11-running.jpeg)

```
            +------------------------------+
            |  RT-11SJ V05.07              |
            |                              |
            |  .DIR                        |
            |  RT11 .SYS    79 12-Jun-77  ... |
            |  ...                         |
            +------------------------------+
                 ESP32-S3 / ILI9341 TFT
                 + Telnet + USB-Serial
```

The active CPU/MMU path is the [**kek**](https://github.com/folkertvanheusden/kek)
PDP-11/70 engine, adapted to the Freenove ESP32-S3 host. The host scaffolding
(TFT console, Telnet server, FTP server, dual-core split, SD-backed block I/O,
`/wificonfig.ini` + `/pdpconfig.ini`, capacitive-touch settings menu, WS2812
status LED) is inherited from `vpdp1140` and remains the board-facing side of
this project.

## Current Bring-Up Status

As of **2026-08-03 (V2.5)**. The sketch reports the `kek PDP-11/70 adapter`
with up to 4 MB target memory. Sample configs live under `PdpSdCard/pdpconfig-*.ini`.

| Config / Name     | Drive | Status  | OS                    | Notes |
|-------------------|-------|---------|-----------------------|-------|
| RSX11M46          | RL02  | working | RSX-11M 4.6           | `boot_script` |
| RT11V5            | RK05  | working | RT-11 5.04            | `boot_input` / typeahead |
| UNIX6             | RK05  | working | Unix V6               | `boot_script` |
| UNIX7             | RL02  | working | Unix V7               | `boot_script` |
| XXDP22            | RL02  | working | XXDP 2.2              | `boot_script` |
| XXDP25            | RL02  | working | XXDP 2.5              | boot script not needed |
| 11MARK            | RL02  | working | RSX-11M 4.8           | `boot_script` |
| 211BSD            | RL02  | working | 2.11BSD #3            | needs `boot_script` |
| DOSBATCH11        | RK05  | working | V10-01A               | needs `boot_script` |
| RSTSV4B           | RK05  | working | RSTS V04B-17          | `boot_input` working |
| RSX11M            | RL02  | working | RSX-11M V4.0          | 28KW baseline; script not needed |
| RSX11MP46-PIDP    | DU0   | working | RSX-11M+ 4.6          | `boot_script` |
| BSD211-PiDP       | DU0   | crash   | —                     | odd PC at `10067` |
| 211BSD-RP0        | RP0   | working | 2.11BSD #5            | needs `boot_script` |
| RSTSV7            | RL01  | working | RSTS/E V7             | Ready (LP11 interrupts; print → `/LPn.TXT`) |
| RSTSV7-FULL       | RL01  | working | RSTS/E V7             | same as RSTSV7 |

Working notes and older history: [`status.txt`](status.txt).

## Hardware

Supported hosts (select with `VPDP_BOARD` in `config.h` — must be set there,
not in the `.ino`, so every `.cpp` sees the same value):

- **Freenove ESP32-S3 2.8" Display** (`VPDP_BOARD_FREENOVE_28`, default production):
  ILI9341 TFT, FT6336U capacitive touch, micro-SD (SD_MMC 4-bit), 8 MB Octal
  PSRAM, 16 MB flash, WS2812 status LED.
- **Elecrow CrowPanel Advance 7"** (`VPDP_BOARD_CROWPANEL_7`): SC7277 RGB
  800×480 via LovyanGFX, GT911 touch (via `LGFX::getTouch`), SPI TF slot
  (DIP S1/S0 must be TF), STC8H backlight, 8 MB OPI PSRAM. The settings menu
  still lays out in the Freenove 320×240 top-left region on this panel.

### Emulation speed / PSRAM

Guest RAM is allocated in ESP32-S3 PSRAM on both boards. Measured kek
microbenchmarks on the Freenove (after `TURBO` / `-O2` / `-DNDEBUG`) land
around **0.4+ MIPS** for MMU-mapped code. The **Elecrow CrowPanel runs
noticeably slower** with the same firmware: its RGB panel continuously DMA-
streams the LovyanGFX framebuffer from the **same OPI PSRAM** that holds
guest memory, so instruction fetches and operand accesses compete with
display refresh bandwidth. The Freenove SPI TFT does not keep a full-frame
PSRAM DMA stream running, so it is the cleaner performance baseline.
Keep diagnostic / panic-trace rings off for speed runs; they cost several×.

Build flags that affect the hot path live in `_upstream_kek/gen.h` (`TURBO`
strips kek `DOLOG`) and sketch-root `build_opt.h` (`-DNDEBUG`, `-O2`).
Optional startup microbenchmarks are gated by `VPDP1170_STARTUP_BENCHMARK`
in `config.h`.

For reproducible full-OS timing, prompt rules, continuous COM capture, and the
shared-cache measurements, see the [operating-system boot benchmark notes](docs/boot-benchmark.md).

The ESP32-S3 dual cores are pinned so display work never stalls the PDP-11
(and guest disk I/O never stalls the TFT).

### Core 0 — display and lightweight network

| Task / component | Role |
|------------------|------|
| `render_task` | Touch poll, console paint (`console_render`), status bar, settings-menu draw |
| `net_task` | Telnet session poll and WiFi reconnect check |

Core 0 does **not** run the PDP-11 CPU or perform guest SD disk I/O.

### Core 1 — emulator and storage (`loop()`)

| Component | Role |
|-----------|------|
| PDP-11 CPU (`pdp_core::run`) | Instruction slices |
| Disk / SD | RL / RK / RP image I/O for the guest |
| Console drain | KL11 → ANSI parser → cell grid |
| USB serial | Host keyboard in; KL11 serial out |
| FTP | File transfer against the SD root (kept here so it does not race guest disk) |
| Telnet shell / emu control | Monitor commands, pause / step, etc. |
| Menu actions | Remount, emulator restart, ESP restart (after UI sets flags on core 0) |

**Hand-off:** core 1 fills the 80×25 cell grid; core 0 takes a coherent
snapshot under a short spinlock and draws it. Settings-menu state is shared
under `g_ui_mutex`.

## Emulated configuration

| Component       | What we emulate                                                   |
|-----------------|-------------------------------------------------------------------|
| CPU             | PDP-11/70 via `kek`; inherited 11/40 scaffold remains for reference |
| Memory          | Target: 4 MB PSRAM-backed 22-bit physical memory                  |
| MMU             | Target: PDP-11/70 22-bit MMU with kernel/supervisor/user spaces   |
| Console         | KL11 UART at `0o177560` (vector 060), bridged to TFT+Telnet+USB    |
| RK05 disk       | RK11 controller at `0o177400` (vector 220), up to 4 drives        |
| RL01/02 disk    | RL11 controller at `0o174400` (vector 160), up to 4 drives        |
| RP04/05/06 disk | RH11 controller at `0o176700` (vector 254), RP0 as secondary disk; testing mode, not yet verified |
| Line clock      | KW11-L at `0o177546` (vector 100), tickrate ~60 Hz                |
| Programmable clock | KW11-P at `0o172540` (vector 104, BR6), four rates, repeat/one-shot |
| Boot ROM        | DEC M9312-style RK0 / RL0 stubs (selected by `boot=` in config)   |

The status bar below the 80×25 console shows drive activity, WiFi IP,
Telnet / FTP state and MIPS in real time.

## Building

Arduino IDE with the ESP32 board package and these libraries:

- **TFT_eSPI** — Freenove only; enable the `FNK0104B` setup in `User_Setup_Select.h`
- **FT6336U** — Freenove-bundled touch library
- **Freenove_WS2812_Lib_for_ESP32** — Freenove only
- **LovyanGFX** — CrowPanel / Elecrow only

The sam11 sources we use are copied directly into the sketch root (see
the file list below) so the Arduino IDE picks them up automatically. No
SdFat library is needed — we route all sam11 disk I/O through our own
`disk.cpp` block layer.

### Tools-menu settings (important)

Common to both boards:

| Setting            | Value                                |
|--------------------|--------------------------------------|
| Board              | **ESP32S3 Dev Module** (not "Octal") |
| Flash Size         | **16MB (128Mb)**                     |
| Partition Scheme   | Huge APP (3MB No OTA / 1MB SPIFFS)  |
| PSRAM              | **OPI PSRAM** (not QSPI or Disabled) |

The CrowPanel Advance 7" uses an **ESP32-S3-WROOM-1-N16R8** module:
**16 MB flash** and **8 MB OPI (octal) PSRAM**. It is not a QSPI-PSRAM board.
Set **Tools → Flash Size → 16MB (128Mb)** every time — a smaller flash size
mis-matches the module and can brick or fail uploads.

OPI PSRAM is mandatory because LovyanGFX stores the 800×480 RGB framebuffer
there. If PSRAM is Disabled or set to QSPI, the boot log reports
`free_psram=0`; framebuffer drawing can then fail with a `StoreProhibited`
panic and reboot the ESP32. That same continuous framebuffer DMA is also why
CrowPanel kek MIPS is lower than Freenove — see **Emulation speed / PSRAM**
above.

**USB CDC On Boot** differs by board — set this every time you switch targets:

| Board | USB CDC On Boot | Why |
|-------|-----------------|-----|
| **Freenove 2.8"** | **Enabled** | App `Serial` is native USB; that is the COM port you monitor. |
| **Elecrow CrowPanel 7"** | **Disabled** | Flash/monitor COM is the USB-UART bridge (UART0). With CDC Enabled, ROM boot text still appears on that port but app `LOG` goes to a different native-USB COM and looks “silent.” |

Select the regular **ESP32S3 Dev Module**, then separately select
**Tools → PSRAM → OPI PSRAM**. Selecting an "…Octal" board variant, or setting
PSRAM to QSPI or Disabled, can bootloop the board.

## SD card layout

```
/wificonfig.ini      WiFi credentials (auto-created if missing)
/pdpconfig.ini       PDP-11 settings (auto-created if missing)
/wificonfig-*.ini    optional WiFi variants picked from the settings menu
/pdpconfig-*.ini     optional PDP variants picked from the settings menu
/unixv6.dsk          V6 Unix RK05 image  (2.5 MB)  ← validated boot
/xxdp25.dsk          XXDP+ diagnostics  (RL02, 10 MB)
/rt11v5.dsk          RT-11 SJ V5  (RK05, optional)
/rsts_full_rl.dsk    RSTS/E V7 boot pack  (RL01, optional)
/rsts_swap_rl.dsk    RSTS/E V7 swap pack  (RL01, optional)
```

Sample images for V6 / XXDP+ / RT-11 / BSD 2.9 / Caldera V5/V6 ship in
sam11's [`OS Images/`](https://gitlab.com/ChloeLunn/sam11/-/tree/master/OS%20Images)
directory.

## Config files

WiFi credentials live in `/wificonfig.ini`; everything else in
`/pdpconfig.ini`. Either file can be missing on first boot — the
firmware writes a default. Drop named copies onto the SD card
(`wificonfig-home.ini`, `pdpconfig-rt11.ini`, ...) and pick one from
the **WiFi Config** / **PDP Config** menu items; selection copies the
chosen variant over the active filename and you get a confirmation
screen offering to reset the ESP32 to apply it.

`/wificonfig.ini`:
```ini
[wifi]
ssid     = YourNetwork        ; blank uses secrets.h defaults
password = YourPassword
hostname = vpdp1170

[ftp]
enabled  = true               ; exposes the SD card root
port     = 21                 ; passive data uses port+1
user     = esp32
password = esp32
```

`/pdpconfig.ini`:
```ini
[telnet]
enabled = true
port    = 23

[console]
boot_input  = ""              ; e.g. "unix\r" or "^CSTART\r" (immediate typeahead)
boot_script = ""              ; e.g. "login: => root\r || Password: => \r"

[serial1]
enabled = false               ; TT1 file-backed DL11 at 0176500

[diag]
pcping      = 5               ; sec between PC dumps; 0 disables
serialdelay = 20              ; ms gate between bursty input chars
io_trace    = 0               ; trace next N I/O-page accesses
clock_trace = 0               ; trace next N clock accesses/IRQs
console_trace = 0             ; trace next N PDP console characters
trace       = false           ; true only for panic/HALT diagnosis
break       = 0               ; octal PC breakpoint before boot; 0 disables
kwp_enabled = false           ; true for RSTS V7 bring-up

[disks]
; dl0..dl3 = RL01/RL02 packs (RL11 controller)
; rk0      = RK05 pack       (RK11 controller)
; rp0      = optional RP04/RP05/RP06 pack (RH11 controller)
; rp0_type = rp04, rp05, or rp06
; rk0 uses a dedicated RK0 slot and does not replace dl0.
dl0  = /xxdp25.dsk
dl1  =
dl2  =
dl3  =
rk0  = /unixv6.dsk
rp0  =
rp0_type = rp06
boot = rk0                    ; or dl0/rl0, dl1/rl1, dl2/rl2, dl3/rl3
```

### RL and RK disk selection

The emulator can boot from either controller:

- `boot = dl0` through `boot = dl3` selects the RL11 bootstrap and treats
  the four disk slots as RL drives `DL0` through `DL3`. RL mounts require
  exact RL01 images of 5,242,880 bytes or exact RL02 images of 10,485,760 bytes.
  `boot = rl0` through `boot = rl3` are accepted as aliases for the same drives.
- `boot = rk0` selects the RK11 bootstrap. The `rk0` image is mounted in a
  dedicated RK0 host slot, separate from `dl0` through `dl3`. RK05 images are
  approximately 2.5 MB; some distributions use paired or combined images of
  approximately 5 MB.

The current drive menu exposes RL units `DL0` through `DL3`, plus the separate
`RK0` image slot. RL mounts are size-checked as RL01 or RL02 packs. RK0 is
kept separate so an RL pack on DL0 is not hidden by an RK boot configuration.

### RP secondary disk

`rp0` mounts one optional RP-family image through an RH11/RP register set at
`0o176700`. Set `rp0_type` to `rp04`, `rp05`, or `rp06` so the controller
reports matching geometry. RP0 support is in testing mode and has not been
verified yet. RP0 is secondary storage only in this build; the boot menu and
M9312-style boot ROM still boot from RK0 or RL0.

## Using it

- The TFT console comes up at boot; the same byte stream is available
  via `telnet <board-ip> 23` and on USB serial (115200 baud).
- `/pdpconfig.ini` `[console] boot_input` can pre-load keystrokes after
  each PDP reboot (typeahead). `boot_script` waits for prompt text
  (case-insensitive) and injects replies: `expect => reply || ...`.
  Both accept escapes such as `\r`, `\n`, `\e`, `\x03`, `\033`, `^C`,
  `^[`, and `^?`.
- The SD card root is available over FTP at `ftp://<board-ip>:21/`
  using the `[ftp]` credentials in `/wificonfig.ini`.
- **Settings menu:** tap the screen or press the onboard button. From
  there you can mount/dismount existing disk images, reboot the PDP-11,
  adjust brightness, and view WiFi / Telnet / FTP status.

### Telnet management shell

While connected by Telnet, press `Esc`, then type `>>` to detach that Telnet
session from the PDP-11 console and enter the emulator management shell:

```text
ESC >>
```

The PDP-11 continues running and remains connected to the TFT and USB serial.
Type `exit` to reconnect Telnet to the PDP console. An incomplete escape
sequence is replayed to the PDP after five seconds.

The shell provides:

```text
pwd
cd <path>
ls [path]
cat <path>
rm <path>
mv <source> <destination>
cp <source> <destination>
drives
mount <RL0-RL3|RK0|RP0> <path> [ro]
dismount <RL0-RL3|RK0|RP0>
create <rk|rl01|rl02> <path>
set [name=value]
monitor
reboot
help
exit
```

File paths may be absolute or relative to the shell's current SD-card
directory. Quote paths containing spaces. Destructive file commands reject
mounted disk images, and `mount` requires the target drive to be dismounted
first. `cat` displays at most the first 100 lines and rejects binary files. The
guest operating system must flush and offline a drive before it is
dismounted. `create` makes zero-filled RK05 (2,494,464-byte), RL01
(5,242,880-byte), or RL02 (10,485,760-byte) images.
RL `mount` accepts only the two exact RL pack sizes: 5,242,880 bytes for RL01
or 10,485,760 bytes for RL02.

`set` with no arguments displays the runtime-changeable settings. Supported
assignments are `pcping`, `serialdelay`, `io_trace`, `clock_trace`,
`console_trace`, `trace`, `break`, `title`, `boot_input`, and `boot_script`;
`boot_text` is accepted as an alias for `boot_input`. For example:

```text
set pcping=1
set io_trace=100
set clock_trace=100
set console_trace=100
set trace=false
set break=04642
set boot_input="hello\r"
set boot_script="login: => root\r"
```

These changes are not written to `/pdpconfig.ini` and are lost when the ESP32
restarts. `boot_input` and `boot_script` take effect on the next PDP-11 reboot. `break` is also
readable from `[diag] break=` in `/pdpconfig.ini` so it can be armed before
early boot.

The `monitor` command enters a front-panel-style PDP-11 monitor. Addresses and
values are octal:

```text
P                     pause after the current instruction
S                     execute one instruction and remain paused
C                     continue execution
D00100                dump physical RAM (alias: MP00100)
D00100:00200          dump an inclusive physical address range
MI00100               dump MMU I-space (current run mode)
MD00100               dump MMU D-space (current run mode; stacks)
T 1000                trace the next 1000 instructions to USB serial
W000100=012345        deposit one word in physical RAM
>                     return to the management shell
```

`P` and `S` display the PC, R0-R5, SP, PSW, and the address, opcode, and
disassembly of the next instruction that `S` will execute. Memory dumps contain eight octal
words and their 16-byte printable ASCII
representation per line. Physical examine/deposit (`D`/`MP`/`W`) accept aligned
22-bit RAM addresses through `017777776`; the PDP-11 I/O page is deliberately
excluded. `MI`/`MD` use 16-bit virtual addresses via the current run-mode I or D
map. Each range dump is limited to 512 words. Leaving monitor mode does not
automatically resume a paused CPU; use `C` when execution should continue.
`T` takes a decimal instruction count; `T 0` cancels an active trace. Trace
lines are written to USB serial in the same register/opcode format as the
panic trace, with disassembly appended.

### Guest-to-emulator control channel

The VPDP command channel on TTY0 is always available. When `[serial1]
enabled = true`, TT1 is additionally exposed as a DL11-compatible serial port
at `0176500` using receive vector `0300` and transmit vector `0304`. A PDP
program can access SD-card files directly through commands regardless of the
TT1 setting; enabling TT1 also permits background file streaming through that
emulated serial port. Commands can also change active RK/RL media or reboot the
emulated PDP:

```text
ESC ] VPDP ; command-text ETX-or-EOT
```

In C, for example:

```c
printf("\033]VPDP;OUT;OPEN;/results.txt;APPEND;REPLY\003");
```

RSTS/E BASIC-PLUS displays `CHR$(27)` as `$` on its console. The emulator
therefore also accepts `$]VPDP;` as a compatibility prefix. For example:

```text
PRINT CHR$(27);"]VPDP;OUT;OPEN;/TEST1.DAT";CHR$(3)
```

The tested BASIC-PLUS form above opens `/TEST1.DAT` in append mode. Add
`;REPLY` before `CHR$(3)` to return a framed acknowledgement to the PDP
program.

The complete frame is intercepted and is not shown on the TFT, USB serial, or
Telnet. Command text is limited to 256 bytes. Printable characters plus CR,
LF, BEL, and TAB are accepted. ETX (`\003`) or EOT (`\004`) executes it; any
other non-printable character aborts it. Formatting controls are removed from
path arguments but remain unchanged in `OUTASCII` data.
Commands requesting `REPLY` receive an ETX-terminated frame in the KL11 input
queue using the same format.

Common commands:

```text
IN;OPEN;/commands.txt;EOF=0x04;NOTIFY;REPLY
IN;CLOSE;REPLY
OUT;OPEN;/results.txt;APPEND;REPLY
OUT;CLOSE;REPLY
TTY;STATUS;REPLY
OUTASCII;data written exactly, including CR/LF/BEL/TAB
OUTHEX;0001027F80FF
INASCII
INHEX:32
DISK;MOUNT;RL1;/new.dsk;REPLY
DISK;DISMOUNT;RL1;REPLY
DISK;STATUS;ALL;REPLY
PDP;REBOOT;COLD;REPLY
```

The direct `OUTASCII` and `OUTHEX` commands write to the currently connected
TT1 output file and flush it before returning. `OUTHEX` converts hexadecimal
pairs to binary bytes and permits spaces between pairs. Prefix the payload
with `REPLY;` to request an acknowledgement.

`INASCII` returns the next input-file line over TTY0, followed by a carriage
return.
`INHEX:n` reads up to `n` bytes (`1` through `128`) and returns uppercase
hexadecimal followed by a carriage return. If fewer than `n` bytes remain,
the response contains those bytes; the following read returns `*>EOF<*`.
Every input response, including `*>EOF<*` and errors, is terminated by a
carriage return.
Direct reads share the TT1 input position; do not have a TT1 driver consume
the same stream concurrently.

Runtime disk changes do not rewrite `pdpconfig.ini`. Before dismounting, the
guest OS must flush and offline/dismount the device using its OS-specific
command. RP0 runtime commands are not supported by this interface.

### Booting V6 Unix

1. Set `boot = rk0` and `rk0 = /unixv6.dsk` in `/pdpconfig.ini`.
2. Power the board. After the WiFi line you should see:
   ```
   vpdp1170: booting PDP-11/70 from RK0...
   @
   ```
3. Type `unix` and Enter — the V6 kernel loads and drops you at `#`.
4. Try `ls /`, `date`, `cat /etc/passwd`, even `cc hello.c`. The
   PDP-11 C compiler is on the disk.

### Booting XXDP+

1. Set `boot = dl0` and `dl0 = /xxdp25.dsk`.
2. After reset you'll see the XXDP-SM monitor prompt `.`. Try
   `R FKAAC0` for the basic instruction-set diagnostic.

## Inherited vpdp1140 Core Still Present

| File                          | Role                                                |
|-------------------------------|-----------------------------------------------------|
| `kd11.cpp` / `.h`             | Inherited PDP-11/40-derived CPU scaffold, retained for reference |
| `kt11.cpp` / `.h`             | Inherited 18-bit MMU scaffold                       |
| `ms11.cpp` / `.h`             | RAM controller — routed to our PSRAM block          |
| `dd11.cpp` / `.h`             | UNIBUS backplane, I/O page dispatch                 |
| `kl11.cpp` / `.h`             | KL11 console — rewired to TFT+Telnet+USB           |
| `telnet_shell.cpp` / `.h`     | Telnet management shell and SD-card commands        |
| `rk11.cpp` / `.h`             | RK11 controller — rewired to `disk.cpp`             |
| `rl11.cpp` / `.h`             | RL11 controller (fresh implementation; not sam11's) |
| `rh11.cpp` / `.h`             | RH11/RP04-RP06 secondary disk controller            |
| `kw11.cpp` / `.h`             | KW11-L line clock                                   |
| `cpu/cpu_*.cpp.h`             | Instruction implementations                         |
| `pdp1140.h`                   | Legacy device addresses, trap vectors, build flags  |
| `bootrom.h`                   | M9312-style RK0 / RL0 boot ROMs                     |
| `sam11_platform.h`            | Our ESP32-S3 platform shim (replaces sam11's)       |

This section describes the inherited baseline only. The `vpdp1170` target is
to replace PDP-visible CPU/MMU/bus/device behavior with `kek` while retaining
the ESP32 host services.

The aggregated sam11 source originally vendored in `_upstream_sam11/`
was copied to the sketch root and edited for `vpdp1140`.
The most material changes are:

- **14 instruction-correctness fixes** in `cpu/cpu_instr.cpp.h` (INC, ROR,
  SWAB, ADD, SUB, NEG, ADC, SBC, ASR, MUL, SXT, MARK, CCC, SBC) — found
  by running XXDP+ FKAAC0.
- **3 new instructions** added (SPL, MTPS, MFPS) — needed for XXDP's
  FKABD0 trap test.
- **PSRAM-backed `ms11`** — sam11's stock allocator wanted a 248 KiB
  array in DRAM, which is most of the ESP32's RAM.
- **Custom `rl11.cpp`** — sam11's stock RL11 is WIP and didn't drive
  XXDP+; rewrote from scratch using the same DEC RL11 manual.
- **Deferred RK11 done-IRQ** in `rk11.cpp` — sam11's stock fires the IRQ
  synchronously inside the controller-register write, which beats the
  guest's `MOV cmd,RKCS / WAIT` pattern and hangs RT-11. We delay by
  ~256 host steps so the WAIT runs first.

## Milestones

- ✅ **m0** Fork v8088, rename / strip down, sketch compiles
- ✅ **m1** Vendor sam11, in-sketch CPU self-test passes
- ✅ **m2** KL11 console on TFT + Telnet + USB-Serial
- ✅ **m3** RL11 → XXDP+ boots; 14 sam11 CPU bugs fixed; SPL/MTPS/MFPS added
- ✅ **m4** V6 Unix boots from RK05 to `#` prompt
- ⏸ **m5** — m4's V6 boot is what m5 was supposed to add (XXDP); already done in m3
- **Settings menu:** mount or dismount existing images on DL0..DL3 and RK0.
- ⏳ **m7** KW11-L line clock — present, but tickrate could be calibrated
- ⏳ **m8** Second DL11 tunneling SD file I/O — designed in chat, not yet built
- ⏳ **m9** RT-11 / RSTS chase — sam11 known-broken; deep-dive if motivated
- ✅ **m10** README polish + GitHub push  ← **this commit**

## Credits

- CPU core: **sam11** by Chloe Lunn — BSD-3-Clause —
  https://gitlab.com/ChloeLunn/sam11
- sam11 descends from Julius Schmidt's JavaScript PDP-11 emulator and
  Dave Cheney's [avr11](https://dave.cheney.net/2014/01/23/avr11-simulating-minicomputers-on-microcontrollers).
- Host scaffolding: ESP32-S3 TFT/Telnet/SD/dual-core stack forked from
  [v8088](https://github.com/deangi/vMSDOS), which itself ports Adrian
  Cable's [8086tiny](https://github.com/adriancable/8086tiny).
- 4×8 console font: public-domain IBM VGA font (via dhepper/font8x8).
- Sample disk images: sam11's `OS Images/` and
  https://www.pcjs.org/software/dec/pdp11/disks/rl02k/xxdp/

## License

vpdp1170 itself is provided under the same license as the upstream
sam11 code it builds on: **BSD 3-Clause**. See `LICENSE` for the full
text. The vendored sam11 sources retain their original copyright notice
(Copyright 2021 Chloe Lunn).
