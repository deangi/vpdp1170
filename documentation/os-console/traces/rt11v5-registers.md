# rt11v5 KL11 register activity

Parsed from `rt11v5-console.log`.

## Summary

- Total kek TTY events: **1000**
- CONSOLE char lines: **0**
- Trace ended marker: **False**

### By action

| Action | Count |
|--------|------:|
| READ | 874 |
| WRITE | 63 |
| TXREADY | 62 |
| RXREADY | 1 |

### By register

| Reg | Count |
|-----|------:|
| TPS | 936 |
| TPB | 63 |
| TKB | 1 |

### Action x register

| Pair | Count |
|------|------:|
| READ TPS | 874 |
| WRITE TPB | 63 |
| TXREADY TPS | 62 |
| RXREADY TKB | 1 |

### IRQ details

_None._

### Notable CSR traffic

- TKS WRITE values (first): `none`
- TPS WRITE values (first): `none`
- TKS READ count: 0
- TPS READ count: 874
- TKB READ count: 0

## Sample events (first 40)

- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000200 remaining=999`
- `[vpdp1170] kek TTY WRITE    TPB @ 177566 val=000166 ch=166 remaining=998`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=997`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=996`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=995`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=994`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=993`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=992`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=991`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=990`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=989`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=988`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=987`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=986`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=985`
- `[vpdp1170] kek TTY TXREADY  TPS @ 177564 val=000200 remaining=984`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000200 remaining=983`
- `[vpdp1170] kek TTY WRITE    TPB @ 177566 val=000160 ch=160 remaining=982`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=981`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=980`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=979`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=978`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=977`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=976`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=975`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=974`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=973`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=972`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=971`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=970`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=969`
- `[vpdp1170] kek TTY TXREADY  TPS @ 177564 val=000200 remaining=968`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000200 remaining=967`
- `[vpdp1170] kek TTY WRITE    TPB @ 177566 val=000144 ch=144 remaining=966`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=965`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=964`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=963`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=962`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=961`
- `[vpdp1170] kek TTY READ     TPS @ 177564 val=000000 remaining=960`
- ... (960 more)
