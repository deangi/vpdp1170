# KL11 console_trace by OS

Live captures via 	ools/capture_kl11_boot.py (COM18 @ 115200, FTP 192.168.7.144, console_trace = 1000).

Raw logs and per-OS register digests: documentation/os-console/traces/.

## Phase 2 baseline (pre-kek_src_tty)

| OS | Status | kek TTY events | READ | WRITE | IRQ | Dominant detail |
|----|--------|---------------:|-----:|------:|----:|-----------------|
| rstsv4b | OK | 1000 | 0 | 0 | 1000 | unqueue vec=064 x1000 |
| rt11v5 | OK | 1000 | 0 | 0 | 1000 | unqueue vec=064 x1000 |
| unixv6 | OK | 1000 | 0 | 0 | 1000 | unqueue vec=064 x1000 |
| rsx11m | OK | 1000 | 0 | 0 | 1000 | unqueue vec=064 x1000 |
| 11mark | OK | 1000 | 0 | 0 | 1000 | unqueue vec=064 x1000 |

Every OS hit kek TTY trace ended with:

    tx=0 txready=0 irq64 q/u=0/1000 rx=0 irq60 q/u=0/0
    TKS=000000 TKB=000000 TPS=000200

**Finding:** TX IRQ unqueue storm burned the entire console_trace=1000 budget before any guest READ/WRITE was logged (irq64 q/u=0/1000).

## Post kek_src_tty flash (2026-07-10)

Firmware on COM18 after flash of DEC-faithful kek_src_tty (banner still reports pdp1170 vV1.1 build 2026-07-02). Re-ran:

`	ext
python tools/capture_kl11_boot.py --os rstsv4b
python tools/capture_kl11_boot.py --os rt11v5
python tools/capture_kl11_boot.py --os unixv6
`

### Capture status

| OS | Status | kek TTY events | READ | WRITE | IRQ | TXREADY | RXREADY | unqueue |
|----|--------|---------------:|-----:|------:|----:|--------:|--------:|--------:|
| rstsv4b | OK | 1000 | 874 | 63 | **0** | 62 | 1 | **0** |
| rt11v5 | OK | 1000 | 874 | 63 | **0** | 62 | 1 | **0** |
| unixv6 | OK | 1000 | 874 | 63 | **0** | 62 | 1 | **0** |

### Cross-OS TTY finding (major pass)

- **Unqueue storm is gone:** IRQ=0 / unqueue=0 in all three 30s captures (was IRQ=1000 / unqueue=1000).
- **CSR READ/WRITE visible:** dominant mix is READ TPS (874) + WRITE TPB (63) + TXREADY TPS (62).
- **irq64 q/u stats:** not present in these logs (no kek TTY trace ended marker; budget exhausted on READ/WRITE/TXREADY before end-of-trace summary).
- Identical CSR counters across OSes: the 1000-event window is consumed by early polled host/adapter TX (pdp1170: kek PDP-11/70 adapter console OK; trying RK0 boot... ≈ 63 TPB chars) before guest-specific CSR patterns fill the trace. Guest boot text still appears later on the serial console outside the counted kek-TTY event mix.

### Per-OS guest evidence

#### rstsv4b

- Digests: 	races/rstsv4b-registers.md / 	races/rstsv4b-console.log
- Guest boot text: **yes** — RSTS V04B-17 RSTSV4B, date/time prompts, RSTS4B - SYSTEM PACK MOUNTED, INIT/Ready
- Prompt: reaches Ready (not a classic . / @)
- **KB01:** still logs DISABLING INTERFACE FOR KB01: during INIT (full RSTS pass criterion not met from this capture alone)
- IRQ: none; no irq64 q/u line

#### rt11v5

- Digests: 	races/rt11v5-registers.md / 	races/rt11v5-console.log
- Guest boot text: **yes** — RT-11FB  V05.04 F, install/help text, device list
- Prompt: **.** present at end of capture
- No KB01 noise
- IRQ: none; no irq64 q/u line

#### unixv6

- Digests: 	races/unixv6-registers.md / 	races/unixv6-console.log
- Guest boot text: **yes** — @unix then login:
- Prompt evidence: past @ to **login:** within the 30s window (interactive password/# not proven from capture alone)
- IRQ: none; no irq64 q/u line

### Verdict vs Phase 2

| Check | Phase 2 | Post flash |
|-------|---------|------------|
| Unqueue storm | FAIL (1000x) | **PASS** (0) |
| CSR READ/WRITE in window | FAIL (0/0) | **PASS** (874/63) |
| Guest boot text on console | blocked / not in TTY trace | **PASS** (all three) |
| Full interactive login / MIPS | n/a | not proven from 30s capture alone |

**Major pass for the TTY rewrite:** storm gone + CSR traffic restored. Remaining OS polish (RSTS KB01 false disable, live input, MIPS) is separate from the IRQ-unqueue hazard.
