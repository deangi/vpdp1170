# KL11/DL11 CSR & IRQ semantics comparison

Scope: console-class SLU at `177560`–`177566` (TKS/TKB/TPS/TPB), plus SIMH
extra-line DL11 where it clarifies the same bit rules.

Bit conventions used below:

- **DONE / RDY** = CSR bit 7 (`0200`)
- **IE** = CSR bit 6 (`0100`)
- RX vector `060`, TX vector `064`, BR4

---

## Comparison matrix

| Behavior | open-simh TTI/TTO (`pdp11_stddev.c`) | open-simh DLI/DLO (`pdp11_dl.c`) | vpdp1140 `kl11` | aap/pdp11 `kl11` | kek-upstream `tty` | python-pdp11 `kl11` |
|----------|--------------------------------------|----------------------------------|-----------------|------------------|--------------------|---------------------|
| **DONE/IE storage** | Separate `tti_csr` / `tto_csr`; DONE and IE live in those words | Per-line `dli_csr[]` / `dlo_csr[]` | `TKS` / `TPS` uint16 | Separate bools (`rdr_done`, `*_int_enab`, `pun_ready`) | `registers[4]`; DONE/IE in word | Separate bools (`rcdone`, `*_ienable`); TX RDY implicit |
| **CSR read mask** | Returns only `DONE\|IE` (`TTICSR_IMP` / `TTOCSR_IMP`) | RX: `DONE\|IE` (+ modem bits if DL11-E); TX: `DONE\|IE\|MAINT\|XBR` | Returns full `TKS`/`TPS` (only bits 7/6 actively managed) | Assembles word from bools; RX also exposes BUSY≪11; TX MAINT≪2 | Returns register word (TKS DONE refreshed from host poll on read) | Assembles IE + DONE; TX always ORs `TXRDY` |
| **High byte (15/14) mirror of DONE/IE?** | **No** on console read (ERR bit 15 exists in defs but not in IMP mask) | Modem/status high bits on DL11-E RCSR only — **not** a DONE mirror | **No** | **No** | **No** (high byte can be written via byte lane, not used as DONE mirror) | **No** |
| **TKS write preserves** | Only IE writable (`TTICSR_RW`); DONE preserved | Only IE (+ modem control bits); DONE preserved | Only IE bit updated; DONE preserved | IE (+ RDR ENAB bit 0); DONE preserved | `v & IE`, then OR DONE if char pending — **clears other bits** | On RDR ENAB write path: clears DONE/buf, sets IE from write; incomplete vs DEC |
| **TPS write preserves** | Only IE writable; DONE preserved | IE (+ MAINT/XBR); DONE preserved | Only IE; DONE preserved | IE (+ MAINT); READY preserved | `(old & DONE) \| (v & IE)` — DONE preserved, other bits cleared | Only IE stored; RDY always on read |
| **RX DONE set when** | `tti_svc` accepts a kbd char (poll cadence / baud wait) | `dli_svc` after `tmxr_getc`; skips overwrite if DONE held &lt;500 ms | `addchar` after guest cleared DONE, prior RX IRQ drained, optional ms pacing | `svc_kl11` when `ttyinput` returns a char | `notify_rx` / TKS read path when host has char; TKB read may leave DONE if more pending | Input thread waits until `!rcdone`, then sets DONE |
| **RX DONE clear when** | Read TKB | Read RBUF | Read TKB (clears bit 7 only) | Read RBUF | Read TKB (or empty poll) | Read RBUF (or RDR ENAB path) |
| **TX DONE clear when** | Write TTB | Write TBUF | Write TPB | Write punch buf | Write TPB | N/A (always ready; no clear) |
| **TX DONE set when** | `tto_svc` after host output + `wait` delay | `dlo_svc` after mux output + wait | After ~32 polls from TPB write | `svc` after write to host fd | Deferred timer (`TTY_TX_DELAY_US`) / idle path | Always set on CSR read |
| **IRQ assert condition** | `SET_INT` when DONE∧IE (on svc or IE write while DONE already set) | Same via `dli_set_int` / `dlo_set_int` | Level-style update: queue if DONE∧IE else cancel | Sticky `intr_flags` bits set when DONE∧IE | RX: queue if DONE∧IE; TX: re-queue if DONE∧IE and vector not already queued | `simple_irq` when DONE∧IE (RX) or IE rising / every TX write |
| **IRQ clear / IACK** | `CLR_INT` on buf write, IE clear, or buf read (RX); **no device IACK re-check** | **IACK clears line ireq** while DONE may still be set — **no auto re-assert** | `cancelinterrupt` when !(DONE∧IE); table entry removed on accept | `bg_kl11` clears flag when vector delivered | `unqueue` when !(DONE∧IE); TX explicitly re-queues if still DONE∧IE after accept | One-shot queue IRQ; no level re-assert |
| **TX re-assert after CPU accepts vector while READY∧IE still set?** | **No** (pending cleared; needs new SET_INT) | **No** (`dlo_iack` clears ireq) | **Only if** something calls `update_tx_interrupt` again (not automatic on IACK) | **No** until next svc/IE write | **Yes** — `update_tx_interrupt` / `service_deferred` re-queues if still DONE∧IE | **No** (unless IE edge or another TPB write) |
| **Interrupt model** | Sticky request flag (edge-on-condition); cleared on IACK path / CLR_INT | Per-line sticky ireq + master INT; cleared on IACK | Interrupt table queue with cancel (soft level) | Sticky flags cleared on vector grant | Queue with explicit TX level re-assert | Simple priority queue, edge-ish |
| **RX load pacing** | Unit wait / poll; hold DONE up to 500 ms before overwrite | Same 500 ms hold; baud via mux poll | Host FIFO + optional `serial_in_delay_ms` + “no pending vec 060” gate | Coarse svc counter (`NNN==20000`) | Host-driven; DONE gates next char in notify/read paths | Thread blocks until guest clears DONE |

---

## Per-implementation notes

### open-simh (authoritative open reference)

- Console and extra DL11 share the same IE/DONE write rule: writing IE=0
  clears the interrupt request; writing IE=1 while DONE already set **raises**
  the request (`(csr & (DONE|IE)) == DONE` before the update).
- CSR reads never invent high-byte mirrors of DONE/IE for the basic KL11/DL11-A/B
  console case.
- After TX IACK, READY can still be 1 and IE 1, but the interrupt request is
  gone until the next explicit `SET_INT` / `dlo_set_int`. That differs from a
  pure wired-AND level request, but matches decades of working OS console code
  that either transmits again or clears IE in the ISR.

### vpdp1140

- Closest “embedded console” cousin to vpdp1170: explicit DONE∧IE gate,
  cancel-on-false, IE-only CSR writes, TX baud-ish delay, RX pacing to avoid
  `klrint` re-entry.
- Does not mask CSR reads to DONE|IE only (harmless if only those bits are set).

### aap/pdp11

- Small, schematic-minded model; documents itself as “not super accurate.”
- Exposes RDR BUSY and MAINT in CSR reads; interrupt delivery clears sticky
  flags (queue/edge), not continuous level.

### kek-upstream

- Notable for **TX level re-assertion**: after the CPU drains vector 064, if
  TPS still has DONE∧IE, it queues again. That is the most hardware-like TX
  request behavior in this set.
- TKS write replaces the register with IE-only (plus live DONE), which is
  stricter clearing than SIMH’s read-modify of IE alone but same guest-visible
  IE/DONE outcome.

### python-pdp11

- Pedagogical / simplified: transmitter always ready; TX IRQ on IE enable and
  on every TBUF write; no TX DONE clear.
- Useful for RX DONE gating patterns, not as a CSR-fidelity reference.

### BlinkenBone / PiDP / Ersatz-11

- No separate CSR semantics to tabulate; PiDP/BlinkenBone = SIMH; Ersatz-11
  closed.

---

## Recommended hardware-faithful behaviors (consensus)

These appear consistently across SIMH + DEC MicroNote #33 / DL11 manuals, and
are followed by the stronger open emulators (SIMH, vpdp1140, largely kek/aap):

1. **DONE (bit 7) and IE (bit 6) are the only console CSR bits that matter**
   for interrupt and polled I/O. Reads may return only those bits (SIMH) or a
   full word that only those bits are meaningful in.

2. **Do not mirror DONE/IE into bits 15/14 on TKS/TPS.** Bit 15 on *RBUF*
   (not RCSR) is error-summary on DL11-C/D/E; console KL11 RCSR high bits are
   unused/zero (BUSY at bit 11 is the KL11 reader-busy exception).

3. **CSR writes: only IE is writable** for basic KL11/DL11-A/B (plus RDR ENAB
   on RX bit 0 / MAINT on TX bit 2 if implemented). **DONE/READY must be
   preserved across IE writes.**

4. **Interrupt request = DONE ∧ IE** (soft level). Enabling IE while DONE is
   already set must assert the request immediately; clearing IE must drop it.

5. **RX DONE** sets when a character is loaded into TKB; clears on TKB read
   (or INIT). Do not set DONE for a new character until the previous DONE was
   cleared (overwrite/overrun is a separate DL11-C+ concern).

6. **TX READY** clears on TPB write; sets after the emulated shift/host
   completion delay. Idle/reset state is READY=1.

7. **TX IRQ after vector accept:** pure level hardware would keep requesting
   while READY∧IE. SIMH clears the sticky request on IACK and does **not**
   auto-reassert; kek does reassert. For OS compatibility, either is usually
   fine because ISRs write TPB or clear IE. For maximum hardware fidelity,
   **re-evaluate DONE∧IE after IACK (or continuously)** and keep the request
   asserted while both remain set — matching kek’s TX path and real DL11
   request wiring.

8. **RX pacing:** gate host→TKB loads on `!DONE` (all serious impls). Optional
   baud/ms delay and “prior RX vector drained” gates (vpdp1140) are
   host-side protections, not architectural CSR rules, but they match real
   line timing better than instruction-rate bursts.

9. **Interrupt model:** prefer a **level-triggered request derived from
   DONE∧IE** (assert/deassert), not a one-shot edge that can be lost if IE is
   set before DONE. Sticky queues are acceptable if they re-check the
   condition when IE is written and when DONE rises, and clear when the
   condition falls.
