# host_lib (Phase 1)

In-tree ESP32 host components shared by vpdp1170 and (later) vZ80 / VC64 / vApple2 / v8088.

## Arduino compile rule

Arduino IDE compiles **sketch-root** `.cpp` / `.ino` only. Sources under `host_lib/` are pulled in by sketch-root shims:

- `fifo.h` → `#include "host_lib/util/fifo.h"`
- `sd_fs.cpp` → `#include "host_lib/sd/sd_fs.cpp"`
- `host_time.cpp` → `#include "host_lib/time/host_time.cpp"`
- `gfx.h` / `touch.*` / `board_*.h` → `host_lib/gfx`, `host_lib/touch`, `host_lib/board`
- `console.cpp` → `#include "host_lib/console/console.cpp"`
- `lp11_capture.cpp` → `#include "host_lib/capture/lp_capture.cpp"`
- `host_lib_build.cpp` → storage guard, log, shell, TelnetPipe, WiFi/net_task/net_ini, ADM-3A parser
- `host_boot_input_build.cpp` / `host_boot_script_build.cpp` → boot sequencers (separate TUs; do not merge)

Do not add a new build system. When a second emulator links this tree, promote to a sibling Arduino library (`Esp32EmuHost`).

## Modules

| Path | Role |
|------|------|
| `util/fifo.h` | SPSC byte ring |
| `sd/sd_fs.*` | Board-neutral `SD_FS` (SD_MMC or Crow SDSPI) |
| `sd/storage_guard.*` | Recursive SD mutex (`HostSdGuard` / `SD_FTP_StorageGuard`) |
| `log/host_log.*` | `LOG`/`LOGE` + `g_serial_silenced` (`HOST_LOG_TAG`) |
| `time/host_time.*` | UTC SNTP |
| `net/wifi_sta.*` | WiFi STA connect + reconnect |
| `net/net_task.*` | Core-0 poll loop (register Telnet/FTP/NTP/…) |
| `net/net_ini.*` | `[wifi]`/`[ntp]`/`[telnet]`/`[ftp]`/`[dz11]` + variant picker |
| `gfx/gfx.h` | `GfxDisplay` + writeback (TFT_eSPI or LovyanGFX) |
| `touch/touch.*` | FT6336U / GT911 tap poll |
| `board/board_*.h` | Pin / geometry abstraction |
| `console/console.*` | Cell buffer, key FIFO, render, VT100 parser |
| `console/term_adm3a.*` | ADM-3A parser (`HOST_TERM_ADM3A`) |
| `telnet/telnet_pipe.*` | Single-client Telnet + IAC + FIFOs |
| `boot/boot_input.*` | Delayed typeahead (`inject` hook) |
| `boot/boot_script.*` | Expect/reply boot answers (`observe` + `inject`) |
| `capture/lp_capture.*` | Line-printer FIFO → `/LPn.TXT` |
| `shell/shell_core.*` | `shell_register` / dispatch / help |
| `shell/shell_settings.*` | Typed `set` key registry |
| `shell/shell_media.h` | `MediaOps` + `GuestControlOps` |

## Sketch glue (vpdp1170)

1. `#define HOST_LOG_TAG "vpdp1170"` before including `host_lib/log/host_log.h` (via `platform.h`).
2. `telnet_shell_init()` registers FS/media/PDP commands and diag settings.
3. `shell_set_guest_control_ops()` + `shell_set_media_ops()` from vpdp adapters.
4. `host_net_task_add(telnet_poll)` / `ftp_poll` / `host_time_poll` then `host_net_task_start()`.
5. KL11/DZ11 stay in vpdp; they call `TelnetPipe` via `telnet.cpp` / `telnet_dz.cpp`.
6. `kek_lp11` only `lp_capture::push` / `init` / `begin_session`.
7. **vZ80 must call `console_set_personality(HOST_TERM_ADM3A)`** — never the VT100/CSI personality.

## Phase 2 consumers

v8088 (VT100 console), vZ80 (ADM-3A console), VC64 / vApple2 (guest video + key FIFO).
