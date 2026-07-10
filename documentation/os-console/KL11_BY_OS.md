# KL11 console_trace by OS (Phase 2)

Live captures via `tools/capture_kl11_boot.py --all` (COM18 @ 115200, FTP `192.168.7.144`, `console_trace = 1000`).

Raw logs and per-OS register digests: `documentation/os-console/traces/`.

## Capture status

| OS | Status | kek TTY events | READ | WRITE | IRQ | Dominant detail |
|----|--------|---------------:|-----:|------:|----:|-----------------|
| rstsv4b | OK | 1000 | 0 | 0 | 1000 | `unqueue vec=064` x1000 |
| rt11v5 | OK | 1000 | 0 | 0 | 1000 | `unqueue vec=064` x1000 |
| unixv6 | OK | 1000 | 0 | 0 | 1000 | `unqueue vec=064` x1000 |
| rsx11m | OK | 1000 | 0 | 0 | 1000 | `unqueue vec=064` x1000 |
| 11mark | OK | 1000 | 0 | 0 | 1000 | `unqueue vec=064` x1000 |

Every OS hit the `kek TTY trace ended` marker with the same counters:

    tx=0 txready=0 irq64 q/u=0/1000 rx=0 irq60 q/u=0/0
    TKS=000000 TKB=000000 TPS=000200

## Cross-OS finding (primary)

**TX IRQ unqueue storm burns the entire `console_trace=1000` budget before any guest READ/WRITE is logged.**

- 1000x `IRQ TPS @ 177564 val=000200 unqueue vec=064`
- `irq64 q/u=0/1000` — zero queues, one thousand unqueues
- No TPB writes / `tx=0` / `txready=0` / no CONSOLE char lines in the window
- TPS stays `000200` (DONE set) during the storm

This is OS-independent in this capture set: RSTS, RT-11, Unix V6, RSX, and 11mark all look identical at the TTY-trace layer. Guest-specific CSR patterns (Unix `0103`, RSTS KB01, RSX init) were **not observable** because IRQ unqueue events consumed the trace counter first.

Sample (all OSes):

- `[vpdp1170] kek TTY IRQ TPS @ 177564 val=000200 unqueue vec=064 remaining=999`
- same until `remaining=0` / `trace ended`

Per-OS digests: `traces/<os>-registers.md` / `traces/<os>-console.log`.

## Register patterns per OS

### rstsv4b / rt11v5 / unixv6 / rsx11m / 11mark

Same Phase-2 observation for each:

- Action mix: IRQ=1000 only
- No TKS/TPS/TKB/TPB READ or WRITE in the traced window
- RSTS KB01 / Unix RDR ENB / RSX init: **not visible yet** (blocked by IRQ storm)
- Implication: need a follow-up that does not count IRQ unqueue against `console_trace`, raises the budget, or gates IRQ tracing until after first guest CSR access

## Spec gap vs proposed `kek_src_tty` rules

| Rule | Intent | Trace evidence / gap |
|------|--------|----------------------|
| IE-only CSR writes preserve DONE | Writing IE (0100) must not clear bit7 DONE | **Gap:** no CSR WRITE events captured; blocked by IRQ storm |
| Bit 7 only | Software tests DONE via `0200` / TSTB | TPS logged as `000200` during IRQ unqueue (bit7 set); no bit15 seen |
| Cheap TPS read | Polled putchar / RT-11 hammer TPS READ | **Gap:** zero TPS READ in window |
| IE while DONE must IRQ | Enabling IE with DONE already 1 queues vec 064 | **Partial/negative:** `q/u=0/1000` shows unqueue without matching queue |
| No TX requeue storm | Must not endlessly requeue/unqueue vec=064 | **FAIL / confirmed hazard:** 1000 unqueues, 0 queues, 0 TX chars |
| RDR ENB after RBUF (Unix) | After TKB READ, TKS WRITE bit0 | **Gap:** no TKB/TKS traffic captured |
| RSTS KB01 CSR preserve | Probe must not wipe live console CSR | **Gap:** no CSR WRITE traffic under current trace accounting |
| RSX / 11mark from traces | Fill from live RSX patterns | **Same as others:** IRQ-only; no RSX-specific CSR pattern yet |

### Observed snapshot (this run)

- **all five OSes**: TPS READ=0, TKS WRITE n=0, IRQ queue~0 requeue~0 unqueue=1000, TKB READ=0, tx=0

### Recommended next capture tweak

1. Stop charging `console_trace` for IRQ unqueue (or only count queue/requeue + READ/WRITE), or
2. Keep serial open across reboot so early guest CSR traffic is not lost to the closed-port gap, and
3. Re-run `--all` once the TX IRQ unqueue storm is fixed — then fill OS-specific rows above.
