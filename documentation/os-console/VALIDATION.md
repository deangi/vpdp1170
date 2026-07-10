# Phase 5 validation (kek_src_tty)

Status as of 2026-07-10: **blocked on firmware rebuild/flash**. New `kek_src_tty.cpp` is in the sketch tree but is **not** on the board yet.

## Tooling check (this session)

| Check | Result |
|-------|--------|
| `arduino-cli` | **Not installed / not on PATH** — cannot compile or upload from CLI |
| Sketch | `vpdp1170.ino` present; `kek_src_tty.cpp` present (~12 KB, local) |
| COM18 | **Present** — `USB Serial Device` (also COM4/COM9 Bluetooth links) |
| Flash from this environment | **Not possible** without Arduino IDE (or installing `arduino-cli` + board FQBN/port upload) |

## Required before re-validation

1. **Rebuild and flash** firmware in **Arduino IDE** with the sketch folder including `kek_src_tty.cpp` (and related stub changes). Confirm the upload targets the ESP32-S3 on **COM18**.
2. Re-run KL11 boot captures (FTP + serial), at least:

```text
python tools/capture_kl11_boot.py --os rstsv4b
python tools/capture_kl11_boot.py --os rt11v5
python tools/capture_kl11_boot.py --os unixv6
```

Optional: `--all` (also rsx11m, 11mark) once the three primary guests look good.

3. Compare new `documentation/os-console/traces/<os>-*.log|md` against Phase 2 baselines in `KL11_BY_OS.md`.

## Pass criteria (from plan Phase 5)

Cold boot each OS with `console_trace=0`, `kl11_trace=0` for MIPS; use `console_trace=1000` captures to inspect CSR/IRQ behavior.

| Guest | Pass criteria |
|-------|----------------|
| RT-11 V5 | Boot to `.`, live input, idle MIPS restored to ~pre-regression (~0.2 class, not 0.05) |
| RSTS V4B | Past INIT; **no** false `DISABLING INTERFACE FOR KB01:`; live input |
| Unix V6 | Boot past `@` with `boot_input`; live input at `#`; non-zero MIPS; TX works |

Regression: XXDP 2.2 if time permits.

After pass: second GitHub push / PR titled “KL11 re-architect: DEC-faithful kek_src_tty (option 3).”

## Trace expectations (vs Phase 2)

Phase 2 (old firmware) on every OS:

- **1000×** `IRQ TPS @ 177564 … unqueue vec=064`
- `irq64 q/u=0/1000`, `tx=0`, **READ=0 / WRITE=0**
- Trace budget burned before any guest CSR traffic

**After** flashing `kek_src_tty.cpp`, captures should show:

- **No** 1000× unqueue storm consuming the whole budget
- **READ/WRITE CSR traffic** (TKS/TPS/TKB/TPB) visible in the `console_trace` window
- IRQ queue/unqueue counts that make sense with guest IE/DONE (not endless unqueue with zero queue)

## Optional pre-flash capture (old firmware)
### Pre-flash probe result (2026-07-10)

Method: open COM18 @ 115200, pulse DTR to reboot, capture ~20 s (no FTP OS swap; board was on RSX11M config).

| Observation | Value |
|-------------|-------|
| Firmware banner | `vpdp1170 vV1.1 build 2026-07-02` (pre-`kek_src_tty` flash) |
| kek TTY lines | 1001 |
| `unqueue vec=064` | **1000** |
| READ / WRITE | **0 / 0** |
| Trace ended | `tx=0 txready=0 irq64 q/u=0/1000 … TPS=000200` |

**Conclusion:** Running firmware still exhibits the Phase 2 TX IRQ unqueue storm. Rebuild/flash with `kek_src_tty.cpp` is required before Phase 5 pass/fail can be judged.

