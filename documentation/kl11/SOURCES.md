# KL11 / DL11 documentation sources

Primary DEC manuals and related references collected for console serial-line (KL11/DL11) register and interrupt behavior.

## Official DEC PDFs (downloaded)

| File | URL | Covers |
|------|-----|--------|
| `KL11_TeletypeControlManual.pdf` | https://bitsavers.org/pdf/dec/unibus/KL11/KL11_TeletypeControlManual.pdf | Official KL11 Teletype Control manual (DEC-11-HR4C-D). Scanned; no text layer. Programming model for TKS/TKB/TPS/TPB. |
| `KL11_EngrDrws.pdf` | https://bitsavers.org/pdf/dec/unibus/KL11/KL11_EngrDrws.pdf | KL11 engineering drawings (M780 + M105 + M782). |
| `EK-DL11-TM-003_Sep75.pdf` (+ `.txt` extract) | https://bitsavers.org/pdf/dec/unibus/DL11/EK-DL11-TM-003_DL11_Asynchronous_Line_Interface_Manual_Sep75.pdf | **Best bit-level CSR reference.** DL11 Asynchronous Line Interface Manual EK-DL11-TM-003 (Sep 1975). Full RCSR/RBUF/XCSR/XBUF bit tables, INIT, interrupts, byte vs word, KL11-compatible console addresses. |
| `EK-DL11-OP-001_Sep76.pdf` | https://bitsavers.org/pdf/dec/unibus/DL11/EK-DL11-OP-001_Sep76.pdf | DL11 user's / operator's manual EK-DL11-OP-001 (Sep 1976). Scanned. |
| `EK-DL11W-OP-001_May77.pdf` | https://bitsavers.org/pdf/dec/unibus/DL11-W/EK-DL11W-OP-001_May77.pdf | DL11-W (serial + KW11-L clock on one board) user's manual. Same async register model as DL11 for the SLU half. |
| `PDP-11_Handbook_Second_Edition_1970.pdf` (+ `PDP11_Handbook_KL11_excerpt.txt`) | https://bitsavers.org/pdf/dec/pdp11/1120/PDP-11_Handbook_Second_Edition_1970.pdf | Early processor handbook: KL11 bulletin (TKS/TKB/TPS/TPB), vectors 60/64, BR4, RDR ENB/BUSY/DONE, interrupt enable-when-DONE-already-set rule, `TSTB` programming example. |
| `PDP11_PeripheralsHbk_1976.pdf` | https://bitsavers.org/www.computer.museum.uq.edu.au/pdf/EB%2005961%2076%2005A-20%20D%2009-02%2060%20PDP11%20Peripherals%20Handbook%201976.pdf | 1976 Peripherals Handbook (~39 MB). CSR conventions: bit 7 = DONE/READY tested with **TSTB**; bit 15 = Error OR on many devices (not DONE mirror). |

## Secondary / driver references (downloaded HTML)

| File | URL | Covers |
|------|-----|--------|
| `gunkies_KL11.html` | https://gunkies.org/wiki/KL11_Teletype_Control | KL11 overview, address/vector conventions, links to DEC manuals. |
| `gunkies_DL11.html` | https://gunkies.org/wiki/DL11_asynchronous_serial_line_interface | DL11 overview, jumper tables, console vs floating addresses. |
| `chdickman_DL11_notes.html` | https://www.chdickman.com/pdp11/Notes/DL11.shtml | Practical DL11/KL11 notes (baud groups, jumpers, console). |
| `unix_v6_kl.c.html` | https://www.retro11.de/ouxr/u6ed/usr/sys/dmr/kl.c.html | Unix V6 `kl.c` driver (IENABLE\|DSRDY\|RDRENB, RBUF read, RDRENB re-arm). |
| `lions_sect27_terminals.html` | https://warsus.github.io/lions-/lionc/sect0027.html | Lions commentary on DL11/KL11 register bits used by Unix V6. |

## Not downloaded (optional follow-ups)

- KL11 drawings also listed as DEC-11-HR4B-D on gunkies (same content family as `KL11_EngrDrws.pdf`).
- Unibus Interface Manual DEC-11-HIAA-D (general bus/interrupt conventions).
- Paper Tape Software Programming Handbook DEC-11-GGPB-D (referenced by DL11 TM for programming examples).
