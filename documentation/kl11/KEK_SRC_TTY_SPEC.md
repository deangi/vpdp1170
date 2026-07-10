# kek_src_tty.cpp — DEC-faithful KL11 (Option 3)

Greenfield implementation for the kek 11/70 path. Inputs: EK-DL11-TM-003,
open-simh `pdp11_stddev.c`, vpdp1140 `kl11` reference, Phase 2 traces
(`documentation/os-console/KL11_BY_OS.md`).

## Rules locked for v1

| Rule | Behavior |
|------|----------|
| DONE/READY | Bit 7 (`0200`) only; CSR reads mask to DONE\|IE |
| IE | Bit 6 (`0100`); CSR writes update IE only (+ RDR ENB side effect) |
| RDR ENB | TKS write bit 0 clears DONE; bit reads as 0 |
| INIT | TPS READY set; TKS clear; IRQs cancelled |
| IRQ model | SIMH sticky: queue once on DONE∧IE; unqueue only when condition clears; no per-instruction requeue/unqueue storm |
| TPS read | Status only — no IRQ side effects |
| TX pacing | ~32 `service_deferred` polls after TPB write |
| RX pacing | Host poll every 100 instructions + `serialdelay` |
| Host out | `console_feed` + `telnet_write` + `kl11::queue_serial_out` |
| Trace | `console_trace` charges READ/WRITE/queue/RXREADY/TXREADY; **not** unqueue |

## Phase 2 gate

Traces showed 1000× `unqueue vec=064` with TPS=`000200` and zero guest CSR
traffic. Sticky IRQ + not charging unqueue against the budget addresses that.
