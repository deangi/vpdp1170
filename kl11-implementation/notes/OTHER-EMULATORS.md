# Notes on closed / SIMH-derived projects

## Ersatz-11 (dbit.com)
- Commercial closed-source PDP-11 emulator (John Dundas / D Bit).
- Implements console as KL11/DL11-compatible SLU; source not available.
- Public docs emphasize compatibility with DEC console ODT and OS consoles
  at 177560; no published CSR bit-level source to compare.
- Archived product page snapshot saved as e11.html (web.archive.org).

## BlinkenBone / PiDP-11
- Front-panel extensions around open-simh PDP-11; console TTI/TTO and DLI/DLO
  come from the same pdp11_stddev.c / pdp11_dl.c lineage analyzed under simh/.
- No independent KL11 CSR implementation — treat as SIMH for CSR/IRQ semantics.
