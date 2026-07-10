# KL11 / DL11 implementation reference collection

Read-only comparison sources for console SLU (KL11/DL11) CSR and interrupt
semantics. **Do not treat this tree as buildable firmware** — files are
snapshots for analysis only.

Runtime emulator sources under the vpdp1170 project root were **not** modified
while assembling this folder.

## Contents

| Directory | Origin | License | What was copied |
|-----------|--------|---------|-----------------|
| `vpdp1140/` | Local sibling project `../vpdp1140` (sam11-derived ESP32 PDP-11/40) | Modified BSD (Chloe Lunn header in `kl11.h`) | `kl11.cpp`, `kl11.h` only |
| `simh/` | [open-simh/simh](https://github.com/open-simh/simh) `PDP11/` | MIT-style (Robert M Supnik / Authors; see `LICENSE.txt`) | `pdp11_stddev.c` (console TTI/TTO), `pdp11_dl.c` (extra KL11/DL11 lines), `pdp11_defs.h` (CSR bit macros), `LICENSE.txt` |
| `aap-pdp11/` | [aap/pdp11](https://github.com/aap/pdp11) | MIT (`LICENSE`) | `kl11.c`, `kl11.h`, `README.md`, `LICENSE` |
| `kek-upstream/` | Local `_upstream_kek/` (Folkert van Heusden PDP-11) | MIT (file header) | `tty.cpp`, `tty.h` |
| `python-pdp11/` | [outofmbufs/python-pdp11-emulator](https://github.com/outofmbufs/python-pdp11-emulator) | MIT (`LICENSE`) | `kl11.py`, `LICENSE` |
| `notes/` | Secondary references (no independent CSR source) | See files | Ersatz-11 product page snapshot; BlinkenBone/PiDP notes |

## Related projects without independent KL11 CSR code

- **BlinkenBone / PiDP-11** — front-panel wrappers around open-simh; console
  behavior is the SIMH TTI/TTO / DLI/DLO code under `simh/`.
- **Ersatz-11** — commercial closed source; no CSR implementation available.
  See `notes/OTHER-EMULATORS.md` and `notes/e11.html`.

## Hardware references (not vendored)

- DEC DL11 Asynchronous Line Interface manuals (Bitsavers)
- DEC MicroNote #33 — DL-type SLU register bit requirements
- Lions Commentary / Unix V6 `kl.c` — OS driver view of KL/DL-11 (not an emulator)

## Analysis

See [COMPARISON.md](COMPARISON.md) for the CSR/IRQ semantics matrix and
recommended hardware-faithful behaviors.
