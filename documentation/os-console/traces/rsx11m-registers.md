# rsx11m KL11 register activity

Parsed from `rsx11m-console.log`.

## Summary

- Total kek TTY events: **1000**
- CONSOLE char lines: **0**
- Trace ended marker: **True**

### By action

| Action | Count |
|--------|------:|
| IRQ | 1000 |

### By register

| Reg | Count |
|-----|------:|
| TPS | 1000 |

### Action x register

| Pair | Count |
|------|------:|
| IRQ TPS | 1000 |

### IRQ details

| Detail | Count |
|--------|------:|
| `unqueue vec=064` | 1000 |

### Notable CSR traffic

- TKS WRITE values (first): `none`
- TPS WRITE values (first): `none`
- TKS READ count: 0
- TPS READ count: 0
- TKB READ count: 0

## Sample events (first 40)

- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=999`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=998`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=997`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=996`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=995`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=994`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=993`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=992`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=991`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=990`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=989`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=988`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=987`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=986`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=985`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=984`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=983`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=982`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=981`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=980`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=979`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=978`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=977`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=976`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=975`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=974`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=973`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=972`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=971`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=970`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=969`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=968`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=967`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=966`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=965`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=964`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=963`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=962`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=961`
- `[vpdp1170] kek TTY IRQ      TPS @ 177564 val=000200 unqueue vec=064 remaining=960`
- ... (960 more)
