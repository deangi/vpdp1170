# Phase 5 validation (kek_src_tty)

Status as of 2026-07-10: **post-flash captures complete** on COM18. TTY rewrite shows a **major pass** (unqueue storm gone; CSR READ/WRITE restored). Full interactive/MIPS criteria are only partially proven from 30s console_trace=1000 captures.

## Tooling / flash

| Check | Result |
|-------|--------|
| Sketch | kek_src_tty.cpp present; flashed to board (user) |
| COM18 | Used for captures @ 115200 |
| FTP | 192.168.7.144 user esp32/esp32 (wait for tp: listening after reboot) |
| Banner | pdp1170 vV1.1 build 2026-07-02 (build string unchanged; behavior matches new TTY) |

## Captures re-run

`	ext
python tools/capture_kl11_boot.py --os rstsv4b
python tools/capture_kl11_boot.py --os rt11v5
python tools/capture_kl11_boot.py --os unixv6
`

Artifacts: documentation/os-console/traces/<os>-console.log and <os>-registers.md. Summary: KL11_BY_OS.md (Post kek_src_tty flash).

## Pass / fail (capture evidence)

Per plan: cold-boot CSR/IRQ behavior with console_trace=1000. User guidance for this session: if unqueue storm is gone and CSR READ/WRITE appear, that is a **major pass** for the TTY rewrite even without full interactive login from a 30s capture.

| Guest | TTY rewrite (storm / CSR) | Guest boot text | Prompt / notes | Overall |
|-------|---------------------------|-----------------|----------------|---------|
| **RT-11 V5** | **PASS** — IRQ=0, unqueue=0, READ=874, WRITE=63 | **PASS** — RT-11FB V05.04 F + install text | **PASS** — . at end of capture; live input / MIPS not measured here | **PASS** (major; interactive/MIPS TBD) |
| **RSTS V4B** | **PASS** — same CSR mix; no unqueue storm | **PASS** — RSTS V04B-17, INIT, Ready | **FAIL/partial** — still DISABLING INTERFACE FOR KB01:; live input not proven | **PASS** on TTY rewrite; **FAIL** on KB01 criterion |
| **Unix V6** | **PASS** — same CSR mix; no unqueue storm | **PASS** — @unix then login: | **PARTIAL** — past @ to login:; # / live input / MIPS not proven in 30s | **PASS** (major; full login TBD) |

### Trace expectations vs Phase 2

| Expectation after flash | Result |
|-------------------------|--------|
| No 1000x unqueue vec=064 storm | **PASS** (0 unqueue / 0 IRQ on all three) |
| READ/WRITE CSR traffic in window | **PASS** (874 READ / 63 WRITE) |
| Sensible irq64 q/u (not 0/1000) | **N/A in log** — no 	race ended / irq64 q/u line (budget spent on READ/WRITE/TXREADY) |

## Pre-flash probe (same day, for contrast)

| Observation | Pre-flash | Post-flash (rstsv4b/rt11v5/unixv6) |
|-------------|-----------|-------------------------------------|
| unqueue vec=064 | 1000 | 0 |
| READ / WRITE | 0 / 0 | 874 / 63 |
| IRQ | 1000 | 0 |
| Guest console text | blocked by storm | RSTS / RT-11 / Unix visible |

## Next (optional)

- Longer capture or raise/rebalance console_trace so guest CSR (TKS IE, Unix RDR ENB) appears after host banner TX
- Manual live-input + MIPS checks with console_trace=0
- RSTS KB01 false-disable follow-up
- Optional --all (rsx11m, 11mark) and XXDP regression
