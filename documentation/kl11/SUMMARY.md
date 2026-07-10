# KL11 / DL11 register & interrupt summary

Authoritative bit definitions below are taken primarily from **EK-DL11-TM-003** (Sep 1975) and the **PDP-11 Handbook 2nd Ed. (1970)** KL11 bulletin. DL11 is the functional single-board replacement for KL11; console programming model is the same.

---

## 1. Register map (console / “zeroth” unit)

Octal Unibus addresses (18-bit I/O page; often written 77756x):

| Addr | Name (DL11) | Name (KL11 handbook) | Access |
|------|-------------|----------------------|--------|
| 177560 | RCSR | TKS (keyboard/reader status) | R/W (mixed bits) |
| 177562 | RBUF | TKB (keyboard/reader buffer) | Read-only data |
| 177564 | XCSR | TPS (teleprinter/punch status) | R/W (mixed bits) |
| 177566 | XBUF | TPB (teleprinter/punch buffer) | Write-only data |

Additional units: first non-console typically 176500 / vector 300; then consecutive. Console vectors fixed at **060** (receiver) and **064** (transmitter). Priority **BR4** (receiver slightly higher than transmitter; changeable by priority plug).

---

## 2. Receiver CSR (RCSR / TKS) — bit definitions

### Common to KL11 and DL11-A/B/C/D (console-relevant)

| Bit | Name | R/W | Behavior |
|-----|------|-----|----------|
| 15 | *(unused on KL11 / DL11-A–D)* | — | **Reads as 0.** On **DL11-E only**: DATASET INT (modem transition flag; clear-on-read of RCSR). **Not** a copy of DONE. |
| 14–12 | modem (DL11-E) | RO | RING / CLR TO SEND / CAR DET |
| 11 | RCVR ACT (DL11) / BUSY (KL11) | RO | **DL11:** set at center of START bit; cleared by leading edge of RCVR DONE; also cleared by INIT. **KL11:** set when RDR ENB goes to 1; cleared when DONE goes to 1. |
| 10 | SEC REC | RO | DL11-E only |
| 09–08 | unused | — | Read as 0 |
| **07** | **RCVR DONE** | **RO** | Set when a full character is in RBUF (DL11: mid first STOP bit). **Cleared by:** addressing RBUF (DATI *or* DATO/DATOB — any select of RBUF), setting RDR ENB, or INIT. |
| **06** | **RCVR INT ENB** | **R/W** | When set, allows interrupt when DONE sets (and when IE is turned on while DONE already 1 — see §3). Cleared by INIT. |
| 05 | DATASET INT ENB | R/W | DL11-E only; cleared by INIT |
| 04 | unused | — | Read as 0 |
| 03–01 | modem control | R/W | DL11-E (SEC XMIT, REQ TO SEND, DTR). DTR **not** cleared by INIT. |
| **00** | **RDR ENB** | **WO** | Advances ASR paper-tape reader one character; **clears DONE**. Cleared by mid-START of next character, or INIT. **Reads as 0** (load-only). KL11 + DL11-A/C (20 mA Teletype). |

### RBUF (177562)

| Bits | Meaning |
|------|---------|
| 15–12 | DL11-C/D/E: ERROR / OR ERR / FR ERR / P ERR (ERROR = OR of the three). **Not** on KL11 / DL11-A/B. Not tied to interrupt. INIT does not necessarily clear error bits. |
| 11–08 | unused → 0 |
| 07–00 | Received data, right-justified. RO; not cleared by INIT. |

---

## 3. Transmitter CSR (XCSR / TPS) — bit definitions

| Bit | Name | R/W | Behavior |
|-----|------|-----|----------|
| 15–08 | unused | — | **Read as 0** (no DONE/READY mirror into bit 15) |
| **07** | **XMIT RDY** (READY) | **RO** | Set when XBUF can accept a character. **Set by INIT.** Cleared by loading XBUF. |
| **06** | **XMIT INT ENB** | **R/W** | Gates transmitter interrupt; cleared by INIT |
| 05–03 | unused | — | 0 |
| 02 | MAINT | R/W | Loops XMIT serial out → RCVR serial in; cleared by INIT |
| 01 | unused | — | 0 |
| 00 | BREAK | R/W | DL11-C/D/E: continuous space; cleared by INIT. Absent on KL11 / DL11-A/B. |

### XBUF (177566)

| Bits | Meaning |
|------|---------|
| 15–08 | unused |
| 07–00 | Transmit data (right-justified). **Write-only.** |

---

## 4. Interrupt conditions

### Vectors / priority

- Receiver → vector **060** (console); transmitter → **064**.
- BR4; if both request, **receiver wins**.

### Gating (level-style AND, not pure edge)

From PDP-11 Handbook interrupt structure (applies to DONE/READY devices generally) and DL11 TM §4.3:

1. Interrupt request exists while **(DONE/RDY ∧ INT ENB)** is true (and BR granted).
2. **0→1 of DONE/RDY** with IE already set → request.
3. **0→1 of IE** while DONE/RDY already set → request (**“DONE already set when IE enabled”** is real and required).
4. If DONE is cleared **before** the interrupt is granted, **no interrupt occurs** (KL11 handbook keyboard section).

DL11 TM wording emphasizes “setting … initiates an interrupt sequence provided … INT ENB is also set,” which matches the AND model; the handbook explicitly adds the enable-while-already-done case.

### DL11-E extra

Separate dataset interrupt path: DATASET INT ENB (bit 5) ∧ DATASET INT (bit 15), same receiver vector family; service routines must clear/re-enable carefully if multiple sources.

---

## 5. Reset / INIT behavior

INIT sources: programmed **RESET**, console START, power up/down.

| Bit | On INIT |
|-----|---------|
| RCVR DONE | Cleared |
| RCVR INT ENB | Cleared |
| RDR ENB | Cleared |
| RCVR ACT / BUSY | Cleared |
| XMIT RDY | **Set** (transmitter ready after reset) |
| XMIT INT ENB | Cleared |
| MAINT, BREAK | Cleared |
| DTR (DL11-E bit 1) | **Not** cleared by INIT; undefined after power-up |
| RBUF data / error bits | Data not cleared; errors not necessarily cleared |
| Unused / WO bits | Read as 0 |

After INIT, enabling transmitter IE while XMIT RDY is still 1 will immediately request a TX interrupt (common OS pitfall / feature).

---

## 6. Byte vs word access

From DL11 TM address-selection logic:

- Unused and load-only bits **always read as 0**.
- Loading unused or read-only bits has no effect.
- **DATOB to high byte** of any register generates **no** control strobes (`A00=1` byte write ignored for selects).
- Writes that matter are **word or low-byte** (`DATOB` with `A00=0`).
- RCVR DONE clears when RBUF is **addressed** (read *or* write select), not only on successful data read.
- Therefore: **high byte of RCSR/XCSR is not a mirror of the low byte.** Word `TST` / `BMI` on the CSR does **not** see DONE/READY; use **`TSTB`** (test low byte → N reflects bit 7).

Official examples:

- KL11 handbook echo loop: `TSTB TKS` / `TSTB TPS`.
- 1976 Peripherals Handbook: `AGAIN: TSTB CSR` / `BPL AGAIN` for DONE.

---

## 7. KL11 vs DL11 differences (documented)

| Topic | KL11 | DL11 |
|-------|------|------|
| Hardware | M780 dual + M105 + M782 | Single M7800 quad + UART |
| Speeds | Fixed per model (110–2400) | Crystal + dials; up to 9600 (option-dependent) |
| Line | 20 mA (EIA via DE11A) | TTL / EIA / 20 mA by cable/option |
| Bit 11 | BUSY (set by RDR ENB) | RCVR ACT (set by START) |
| RBUF errors | None | C/D/E: overrun/framing/parity + ERROR bit 15 |
| BREAK | No | C/D/E bit 0 of XCSR |
| Modem | No | E option (DATASET INT in RCSR bit 15) |
| Double buffering | Discrete shift regs | UART holding + shift (XMIT RDY returns quickly after first load) |
| Programming model | Same 4 regs, same DONE/IE/RDY bits | Same (A/B Teletype-compatible) |

---

## 8. OS / emulator quirks

### Unix V6 (`kl.c` / Lions)

- Opens with `klrcsr |= IENABLE|DSRDY|RDRENB` and `kltcsr |= IENABLE`.
- `DSRDY` is bit 1 (DTR) for DL11-E carrier; harmless no-op on KL11/DL11-A if unimplemented.
- `RDRENB` re-armed after each RBUF read in `klrint`.
- `ttstart` tests **`(tcsr & DONE) == 0`** with DONE = **0200 (bit 7)**, not the sign bit.
- Comment `/* hardware botch */`: if received char `& 0177 == 0`, write it to XBUF (NUL / framing oddity workaround).

### “Mirror DONE into bit 15” myth

**False for KL11 and DL11-A–D.**

- Official docs: unused high bits read as **0**.
- Bit 15 of RCSR is **DATASET INT** only on DL11-E, unrelated to character DONE.
- Bit 15 of RBUF on DL11-C/D/E is **ERROR**, not DONE.
- Correct poll idiom is **`TSTB` / mask 0200**, not `CSR < 0` / word `BMI` on the status register.
- Some emulators mirror bit 7→15 so buggy software that does word sign tests “works”; that is **not** real KL11/DL11-A/B/C/D console hardware behavior.
- (PC11 paper-tape and many mass-storage CSRs *do* put Error in bit 15 — do not confuse those with KL11.)

### Other emulator traps

1. **INIT must set XMIT RDY**; clearing it on reset breaks TX-after-RESET.
2. Enabling IE with DONE/RDY already set must interrupt.
3. Clearing DONE by RBUF access before BR grant cancels the RX interrupt.
4. RDR ENB is write-only and clears DONE; reading RCSR never shows bit 0 set.
5. Odd-byte writes must be ignored; high-byte reads must be 0 (except real DL11-E/C modem/error bits where defined).
6. KL11 BUSY vs DL11 RCVR ACT timing differs if software inspects bit 11.

---

## 9. Contradiction checklist vs common emulator assumptions

| Assumption | Real hardware (DEC docs) |
|------------|---------------------------|
| Bit 15 of CSR = DONE/READY | **No** (unused=0; DL11-E RCSR.15 = dataset int) |
| Word `TST`/`BMI` polls ready | **No** — use `TSTB` / bit 7 |
| IE enable while DONE set is edge-only / ignored | **No** — must request interrupt |
| INIT clears XMIT RDY | **No** — INIT **sets** XMIT RDY |
| RBUF read-only clear of DONE | Partially wrong — **any address** of RBUF clears DONE |
| High-byte DATOB updates CSR | **No** — no select generated |
| KL11 ≡ DL11-E bit 15 | **No** |
