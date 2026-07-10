# OS Console / KL11–DL11 Interaction Analysis

Focus: how **RT-11 V5**, **RSTS/E V4B**, and **Unix V6** (with brief V7 / 2.11BSD notes) drive the console serial interface at the classic KL11/DL11 register set. Intended for designing a KL11 emulator and planning live-boot instrumentation.

**Documentation only** — no emulator source changes.

---

## 1. Hardware model (shared by all OSes)

### Registers (console = “zeroth” KL11/DL11)

| Addr (16-bit I/O) | Name (old / Lions) | Role |
|-------------------|--------------------|------|
| `0177560` | TKS / RCSR / `klrcsr` | Receiver CSR |
| `0177562` | TKB / RBUF / `klrbuf` | Receiver data |
| `0177564` | TPS / XCSR / `kltcsr` | Transmitter CSR |
| `0177566` | TPB / XBUF / `kltbuf` | Transmitter data |

Vectors: **060** (RX), **064** (TX). Typical BR level: **BR4**.

### Important CSR bits

| Bit | Octal | RX (TKS) | TX (TPS) |
|-----|-------|----------|----------|
| 0 | `000001` | **Reader Enable** (write-1; clears DONE) | — |
| 1 | `000002` | DTR / “data set ready” style (DL11-E etc.) | — |
| 6 | `000100` | **Interrupt Enable** | **Interrupt Enable** |
| 7 | `000200` | **DONE** (char in RBUF) | **READY** (can accept XBUF) |
| 15 | `100000` | Error (often in RBUF bit 15, not RCSR) | Error (device-dependent) |

Citations:

- [Gunkies: KL11](https://gunkies.org/wiki/KL11), [Gunkies: DL11](https://gunkies.org/wiki/DL11_asynchronous_serial_line_interface)
- [joev.org console I/O](https://joev.org/2020-09-16-console-io.html) — documents `TSTB` / `BPL` polling on bit 7, and `INC TKS` to set reader enable
- Lions commentary on register bits: [Interactive Terminals](https://warsus.github.io/lions-/lionc/sect0027.html)
- DEC peripherals handbook interrupt rule: if IE is set, a 0→1 of Done/Ready (or enabling IE while Done/Ready is already 1) requests an interrupt ([Peripherals Handbook excerpt](https://bitsavers.org/www.computer.museum.uq.edu.au/pdf/EB%2005961%2076%2005A-20%20D%2009-02%2060%20PDP11%20Peripherals%20Handbook%201976.pdf); Bob Supnik’s [PDP-11 Interrupts](http://simh.trailing-edge.com/docs/pdp11interrupts.pdf))

### Status-bit test idioms (emulator-critical)

| Idiom | What it tests | Who uses it |
|-------|---------------|-------------|
| `TSTB @#TKS` / `BPL` | **Bit 7** of low byte (`0200`) | Classic polled code, RT-11-style loops, boot ROMs |
| `BIT #200,@#TKS` / `BNE` | Same bit 7 | Explicit mask form |
| `(csr & 0200) == 0` | Bit 7 | Unix V6/V7 C drivers (`DONE`) |
| `TST @#TKS` / `BMI` or `if (klcsr < 0)` | **Bit 15** of the **word** | Rare for DONE; **not** stock V6 `klrint` |

**Note on local project comment** (`kek_src_optional_device_stubs.cpp`: “Unix V6 klrint tests klcsr&lt;0”): stock V6 `klrint` does **not** read or test RCSR for DONE; it assumes the RX interrupt means a character is present. Stock V6/V7 test TX ready with **`DONE` = `0200` (bit 7)**. Mirroring DONE into bit 15 is a defensive emulator aid for word-signed tests, not a requirement of stock V6 `kl.c`.

---

## 2. Unix V6 (primary)

### Sources

- [V6 `kl.c`](https://www.retro11.de/ouxr/u6ed/usr/sys/dmr/kl.c.html)
- [V6 `tty.c`](https://www.retro11.de/ouxr/u6ed/usr/sys/dmr/tty.c.html) / [V6 `tty.h`](https://www.retro11.de/ouxr/u6ed/usr/sys/tty.h.html)
- [V6 `prf.c` `putchar`](https://www.retro11.de/ouxr/u6ed/usr/sys/ken/prf.c.html)
- Lions: [sect 24](https://warsus.github.io/lions-/lionc/sect0027.html), [KL handler](https://warsus.github.io/lions-/lionc/sect0301.html)

### Buffer / I/O model

Three software **clists** per line (`t_rawq`, `t_canq`, `t_outq`):

1. **RX interrupt** (`klrint`) → read RBUF → `|= RDRENB` → `ttyinput` → raw queue; optional **software echo** via `ttyoutput` + `ttstart`.
2. **Canonicalization** (`canon`) moves raw → cooked queue for line-mode reads.
3. **TX** (`ttstart` / `klxint`): if `(tcsr & DONE)==0` return; else `getc(t_outq)` → write XBUF (with parity from `partab`).

Hardware is **one character deep** each way; all buffering is software.

### Polling vs interrupts

| Path | Mode |
|------|------|
| Normal TTY (`/dev/tty*`, console after `klopen`) | **Fully interrupt-driven** (RX + TX IE) |
| Kernel `printf` / `putchar` (`prf.c`) | **Polled TX**: spin on `KL->xsr & 0200`, briefly **clear TX IE** (`xsr = 0`), write char, restore prior `xsr` |

### Init CSR traffic (`klopen`)

```c
addr->klrcsr =| IENABLE|DSRDY|RDRENB;  /* 0100 | 0002 | 0001 = 0103 */
addr->kltcsr =| IENABLE;               /* 0100 */
```

Lions: RCSR pattern **`0103`** = IE + DTR/DSRDY + reader enable; TX IE alone so a ready transmitter immediately interrupts and drains `t_outq`.

### Status bits tested

- TX start: **`DONE` (`0200`)** only — `tty.h`: `#define DONE 0200`, `#define IENABLE 0100`.
- RX ISR: **no CSR DONE test**; read RBUF, then `klrcsr |= RDRENB` (re-arm reader enable / clear DONE per Lions).
- Panic/printf: poll **`0200`** on XCSR; save/restore whole XCSR word.

### Echo, WAIT, SPL

- Default open flags: `XTABS|LCASE|ECHO|CRMOD` → **software echo** from `ttyinput` at interrupt time.
- Top-half wait: `spl5()` around sleep on raw/out queues; `sleep(..., TTIPRI/TTOPRI)`; `spl0()` after.
- Console vectors’ new PS typically encode minor device; ISR runs at device priority (BR4 class).

### Sensitivity (emulator design)

| Hazard | Effect on V6 |
|--------|----------------|
| **Spurious TX IRQ** when IE set and READY already 1 | Expected at open: `klxint` → `ttstart` with empty outq is mostly harmless (no char written). Flooding TX IRQs without clearing READY can spin. |
| **Wiping RCSR** on write (clearing bit 0 / not preserving IE) | Breaks RX re-arm (`|= RDRENB`) and open init (`|= 0103`). Must OR into CSR, not replace blindly. |
| **Reader enable ignored** | After first char, DONE may stick; next RX never completes. V6 **depends** on `|= RDRENB` after every RBUF read. |
| **Null/break in RBUF** | `klrint` writes null to XBUF (“hardware botch”); `putchar` may refuse to print if last RBUF was break/null (V7/2.11BSD; V6 `putchar` uses switch register instead). |
| **putchar clears TX IE** | Emulator must allow XCSR=0 then restore; TX IRQ must not fire while IE cleared mid-printf. |

### Expected live-boot traffic (Unix V6)

1. Early boot / `@` prompt may be standalone or simple polled code.
2. After kernel mounts and `init` opens console: **`TKS |= 0103`**, **`TPS |= 0100`**.
3. Steady state: RX IRQ → read TKB → `TKS |= 1`; echo may write TPB; TX IRQ chain while outq non-empty.
4. Occasional **polled** printf path: tight TPS `0200` waits, TPS save/0/restore.

---

## 3. Unix V7 and 2.11BSD (brief)

### V7 ([`kl.c`](https://minnie.tuhs.org/cgi-bin/utree.pl?file=V7/usr/sys/dev/kl.c))

Same KLADDR / `IENABLE|DSRDY|RDRENB` / TX `IENABLE` open pattern. Differences:

- Dedicated `klstart` (not only shared `ttstart` layout).
- `putchar` still polls `tcsr&0200`, saves/restores TCSR, suppresses output if `(rbuf&0177)==0`.
- Still **bit 7 / `0200`**, interrupt-driven TTY + polled console printf.

### 2.11BSD ([`kl.c`](https://minnie.tuhs.org/cgi-bin/utree.pl?file=2.11BSD/sys/OTHERS/kl/kl.c))

Same architecture with DL11-E modem/baud extensions (`DL_RIE`, `DL_RE`, `DLXCSR_TIE`, `DLXCSR_TRDY`). Console `putchar` still **polls TRDY**, clears TX CSR for the duration of the character, restores. RX path still **`|= DL_RE`** (reader enable) after buffer read when not in modem-special paths. Uses `spl5`/`spl6` around modem and baud updates.

**Instrumentation takeaway:** V7/2.11BSD look like V6 on the wire for the console KL11; expect the same IE/RDRENB init and polled printf dance.

---

## 4. RT-11 V5

### Sources / references

- [RT-11 Device Handlers Manual (V5.6)](https://bitsavers.trailing-edge.com/pdf/dec/pdp11/rt11/v5.6_Aug91/AA-PE7VA-TC_RT-11_Device_Handlers_Manual_Aug91.pdf) — interrupt service structure (`.DRAST`, enter at PR7, lower to device priority)
- Console register tutorial: [joev.org](https://joev.org/2020-09-16-console-io.html)
- Example polled echo: [retrocmp PDP-11/05](https://retrocmp.com/how-tos/interfacing-to-a-pdp-1105/146-interfacing-with-a-pdp-1105-test-programs-and-qhello-worldq) — `TSTB (r0)` / `BPL` on TKS
- Multiterminal / TT handler mods historically used `MOV #100,@#TPS` to enable TX IE ([ibiblio ttmod.mac](http://www.ibiblio.org/pub/academic/computer-science/history/pdp-11/rt/sigtapes/spri76/ttmod.mac))
- Local project note: RT-11 uses **bit 7** status tests (vs word bit 15)

### Buffer / I/O model

- User programs use EMTs **`.TTYIN` / `.TTYOUT` / `.TTINR` / `.TTOUTR`** (and higher-level `.PRINT`, etc.).
- Resident Monitor maintains **console input/output ring buffers**; the console is not a normal queued disk-style handler for every keystroke.
- `TT.SYS` exists as a small terminal-related handler; multiterminal systems add MT hooks (Device Handlers Manual §1.6.3 / §1.12).
- Hardware still one-char deep; OS rings absorb typing and print ahead.

### Polling vs interrupts

| Context | Mode |
|---------|------|
| Running SJ/FB/XM monitor console | **Interrupt-driven** (vec 060/064); monitor fills/drains rings |
| Standalone / bootstrap / many utilities | **`TSTB`/`BPL` poll** on bit 7 |
| User foreground waiting for input | May idle/WAIT while ISR posts characters into the ring |

### Init / IE patterns

Typical monitor/handler pattern (same as other DEC DL11 clients):

- `BIS #100,@#TKS` — enable RX interrupts  
- `BIS #100,@#TPS` — enable TX interrupts  
- Often `INC @#TKS` or `BIS #1,@#TKS` for reader enable when arming RX  
- Abort/disable paths: `BIC #100` on CSR (see Device Handlers Manual interrupt guidelines; DD.MAC-style `CS$INT = 100`)

ISR entry: device interrupts at high PS; `.DRAST` / `.INTEN` **lowers priority** so higher-priority devices can preempt (Handlers Manual §1.2.4).

### Status bits tested

- **Almost always bit 7 via `TSTB`/`BPL` or `BIT #200`**, not signed word bit 15.
- DONE/READY must clear on RBUF read / XBUF write respectively (hardware contract).

### Echo, WAIT, SPL

- Console echo is typically **monitor/terminal service** (half- vs full-duplex SET options), not the KL11 “maintenance” loopback bit.
- Foreground may use WAIT or poll the ring; ISR runs at elevated priority then returns through RMON.

### Sensitivity

| Hazard | Effect on RT-11 |
|--------|-----------------|
| Spurious TX IRQ with empty print ring | ISR should no-op or disable TX IE until next `.TTYOUT`; bad emulators that never drop READY cause IRQ storms and apparent hangs. |
| CSR write replaces entire word | Can clear DONE/READY or IE unexpectedly; RT-11 often uses `BIS`/`BIC` on bit 6 only — emulator should preserve DONE and other bits. |
| Reader enable | Needed for paper-tape/reader semantics on real KL11; many CRT consoles still see software arming with bit 0. Clearing DONE without delivering a char confuses ring logic. |
| Overrun / multi-char host paste | Known to overflow RT-11 keyboard handling (see Pico_1140 KED discussions) — pace RX. |

### Expected live-boot traffic (RT-11 V5)

1. Bootstrap may poll TPS bit 7 while printing banners.
2. Monitor enables **TKS/TPS IE (`0100`)** early.
3. Steady state: RX IRQ → read TKB → post to input ring; TX IRQ → pull from output ring → TPB; IE may be toggled when rings empty/full.
4. Instrumentation: log `BIS`/`BIC` of `0100` and `TSTB` loops separately from IRQ deliveries.

---

## 5. RSTS/E V4B

### Sources / references

- SYSGEN listing shows terminal module **`TT.MAC`** ([RSTS V4B build notes](http://iamvirtual.ca/PDP-11/RSTS-11/RSTS4B-Install.htm))
- Console device **`KB0:`** / control **`TT0:`** at CSR **`777560`** (later RSTS configs; same classic address)
- Boot message **`DISABLING INTERFACE FOR KB01:`** when the next KL11 slot has no live hardware — normal on single-console systems
- Local emulator note: **preserve DONE, reader enable, and other CSR bits on IE writes** (“RSTS KB01 probe”)
- Paul Koning / classiccmp discussions: RSTS terminal driver owns DL11-like ports including console ([cctalk thread](https://www.classiccmp.org/pipermail/cctalk/2021-March/058178.html))

### Buffer / I/O model

- Timesharing **terminal service** (not a simple single-job ring like RT-11 SJ): per-keyboard input buffers, output buffers, echoing, and job attachment (`KB0:` restricted console, etc.).
- BASIC-PLUS and system dialogs go through this service; hardware remains KL11/DL11 one-char FIFOs.

### Polling vs interrupts

- **Interrupt-driven** for production terminal I/O (RX + TX).
- INIT / ODT / very early code may poll.
- At startup, RSTS **probes** configured KL11/DL11 addresses (KB0, KB1, …). Failed probe → “DISABLING INTERFACE FOR KBxx”.

### Init / IE patterns

Expect:

1. Probe reads/writes of TKS/TPS at `177560` and follow-on addresses (`1776500` class for extra KL11s).
2. Enable IE with **`BIS #100`**-style updates that must **not** destroy DONE or reader-enable state the probe left behind.
3. Arm RX (reader enable / DTR bits as required by DL11 variant).
4. TX IE enabled when output is pending; READY already set ⇒ **immediate TX interrupt** (DEC interrupt rule) — driver must tolerate empty-buffer TX IRQs.

### Status bits tested

- Same **`0200` / bit 7** Done/Ready convention as other DEC software.
- Probe logic may do full-word CSR reads; wiping bits on write is a common emulator foot-gun for the KB01 disable path.

### Echo, WAIT, SPL

- **Software echo** inside terminal service (full duplex).
- Job I/O waits in the monitor while ISRs run at device priority; clock and disk IRQs interleave.

### Sensitivity

| Hazard | Effect on RSTS V4B |
|--------|---------------------|
| **CSR write clears reader enable / DONE** | Breaks KB0 and false-fails or false-succeeds KB01 probe; local stub explicitly preserves these bits. |
| Spurious TX interrupts | Terminal service must handle READY+IE with nothing to send; IRQ storm → sluggish or wedged jobs. |
| Missing RX re-arm | Console accepts one character then goes deaf — fatal for `START` / LOGIN dialogs. |
| Extra KL11 at 6500 responding incorrectly | May enable KB01 and steal/misroute traffic. |

### Expected live-boot traffic (RSTS V4B)

1. Burst of CSR **reads** (and careful writes) at `177560` and possibly `176500` during interface scan.
2. Message path for disabled KB01 (no sustained IRQs there).
3. KB0: `TKS`/`TPS` IE enable; dialog output via TX IRQ chain; input via RX IRQ + echo TX.
4. Instrumentation: tag CSR writes that change only IE vs those that rewrite the whole register; watch for probe vs steady-state phases.

---

## 6. Cross-OS comparison (emulator checklist)

| Topic | RT-11 V5 | RSTS V4B | Unix V6 |
|-------|----------|----------|---------|
| Primary mode | IRQ + monitor rings | IRQ + TT service | IRQ + clists |
| Polled path | Boot / some utils | Early INIT | `prf.c` putchar |
| Init TKS | `BIS #100` (+ RE) | Probe then IE/RE | `|= 0103` |
| Init TPS | `BIS #100` | IE when needed | `|= 0100` |
| DONE test | `TSTB` bit 7 | bit 7 | `DONE`=`0200` |
| `klcsr < 0` (bit 15) | No (typical) | No (typical) | **No** in stock `klrint` |
| Echo | Monitor/SET | Terminal service | `ECHO` → `ttyoutput` in ISR |
| SPL / priority | `.DRAST` lower from 7 | Monitor ISR priority | `spl5` around sleeps; ISR at BR4 |
| Needs RDRENB re-arm | Yes (KL11 semantics) | Yes | **Yes** every `klrint` |
| Spurious TX IRQ | Must tolerate | Must tolerate | Tolerated at open |
| CSR bit preserve on IE write | Important | **Critical (KB probe)** | Important (`\|=` style) |

### Hardware interrupt rule (all OSes)

Enabling TX IE while TPS READY is already 1 **must** raise vector 064 (or the OS’s first `ttstart`/`klxint` never runs). Same for RX if DONE already set when IE is enabled. See Supnik’s interrupt paper and DEC peripherals handbook.

---

## 7. Instrumentation plan for live boots

Suggested KL11 trace events (host log):

1. **CSR write** — old/new TKS & TPS; flag bits changed (`IE`, `RE`, `DONE`/`READY`).
2. **CSR read** — especially probe bursts (RSTS) and `TSTB` polls (RT-11 / Unix putchar).
3. **RBUF read / XBUF write** — char value, whether IRQ was pending.
4. **IRQ queue/unqueue** — vec 060/064, PSW priority, whether DONE∧IE true.
5. **Phase markers** — count IE enables; detect “probe” (many CSR touches, few chars) vs “dialog” (steady RX/TX chars).

Per-OS success signatures:

- **RT-11:** after banner, `.` prompt; DIR/SHOW work; TX IRQ rate tracks printing; no IRQ storm when idle.
- **RSTS V4B:** `DISABLING INTERFACE FOR KB01` acceptable; `START` / BASIC dialog echoes; KB0 stays responsive.
- **Unix V6:** after `unix` boot, login/getty on console; typed chars echo; `printf` during panic still works (polled).

---

## 8. Citations (URL list)

| Resource | URL |
|----------|-----|
| Unix V6 `kl.c` | https://www.retro11.de/ouxr/u6ed/usr/sys/dmr/kl.c.html |
| Unix V6 `tty.c` | https://www.retro11.de/ouxr/u6ed/usr/sys/dmr/tty.c.html |
| Unix V6 `tty.h` | https://www.retro11.de/ouxr/u6ed/usr/sys/tty.h.html |
| Unix V6 `prf.c` | https://www.retro11.de/ouxr/u6ed/usr/sys/ken/prf.c.html |
| Lions §24 Interactive Terminals | https://warsus.github.io/lions-/lionc/sect0027.html |
| Lions KL/DL handler | https://warsus.github.io/lions-/lionc/sect0301.html |
| Unix V7 `kl.c` | https://minnie.tuhs.org/cgi-bin/utree.pl?file=V7/usr/sys/dev/kl.c |
| 2.11BSD `kl.c` | https://minnie.tuhs.org/cgi-bin/utree.pl?file=2.11BSD/sys/OTHERS/kl/kl.c |
| KL11 wiki | https://gunkies.org/wiki/KL11 |
| DL11 wiki | https://gunkies.org/wiki/DL11_asynchronous_serial_line_interface |
| Console I/O tutorial (`TSTB`) | https://joev.org/2020-09-16-console-io.html |
| Polled echo example | https://retrocmp.com/how-tos/interfacing-to-a-pdp-1105/146-interfacing-with-a-pdp-1105-test-programs-and-qhello-worldq |
| RT-11 Device Handlers Manual | https://bitsavers.trailing-edge.com/pdf/dec/pdp11/rt11/v5.6_Aug91/AA-PE7VA-TC_RT-11_Device_Handlers_Manual_Aug91.pdf |
| PDP-11 interrupt variations (Supnik) | http://simh.trailing-edge.com/docs/pdp11interrupts.pdf |
| RSTS V4B install / TT.MAC / KB01 message | http://iamvirtual.ca/PDP-11/RSTS-11/RSTS4B-Install.htm |
| Local project notes | `kek_src_optional_device_stubs.cpp` (DONE/IE mirroring; RSTS CSR preserve); `status.txt` (boot status) |

---

## 9. Per-OS summary cards (quick reference)

### RT-11 V5

- **Model:** Monitor rings + IRQ; user EMTs.
- **Init:** `BIS #100` on TKS/TPS; bit-7 polls in standalone code.
- **Tests:** `TSTB` / `0200`.
- **Watch:** TX IRQ when idle; CSR RMW; RX pacing.

### RSTS/E V4B

- **Model:** TT.MAC terminal service + IRQ; KB0 at `177560`.
- **Init:** Interface probe (KB01 disable OK); then IE/RE on live lines.
- **Tests:** bit 7 Done/Ready; full-word CSR reads during probe.
- **Watch:** Preserve CSR bits on IE writes; spurious TX IRQ; RX re-arm.

### Unix V6

- **Model:** clists + `klrint`/`klxint`; software echo; polled `putchar`.
- **Init:** `TKS |= 0103`, `TPS |= 0100`.
- **Tests:** `DONE=0200` in C; no `klcsr<0` in stock `klrint`.
- **Watch:** `|= RDRENB` every RX; putchar save/clear/restore TPS; open-time TX IRQ.
