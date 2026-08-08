#!/usr/bin/env python3
from pathlib import Path
import re

text = Path(
    "boot-benchmark-results-211bsd-postphys/20260802-172605-prearm-firstfault-com.log"
).read_text("latin-1", "replace")
print("size", len(text))
PC_RE = re.compile(
    r"kek trace: PC=([0-7]+)\s+P=([0-7]+)\s+ins=([0-7]+)\s+PS=([0-7]+)\s+"
    r"R0=([0-7]+)\s+R1=([0-7]+)\s+R2=([0-7]+)\s+R3=([0-7]+)\s+"
    r"R4=([0-7]+)\s+R5=([0-7]+)\s+SP=([0-7]+)\s+(.*)$",
    re.M,
)
entries = list(PC_RE.finditer(text))
print("entries", len(entries))
trans = []
for i in range(1, len(entries)):
    ps0 = int(entries[i - 1].group(4), 8)
    ps1 = int(entries[i].group(4), 8)
    u0 = (ps0 & 0o140000) == 0o140000
    u1 = (ps1 & 0o140000) == 0o140000
    if u0 and not u1:
        trans.append(i)
print("user->kernel", len(trans), trans[-8:])
for ti in trans[-2:]:
    print("---", ti, "---")
    for j in range(max(0, ti - 20), min(len(entries), ti + 3)):
        m = entries[j]
        mark = ">>" if j == ti else "  "
        print(
            f"{mark} PC={m.group(1)} PS={m.group(4)} ins={m.group(3)} "
            f"R0={m.group(5)} R1={m.group(6)} R4={m.group(9)} SP={m.group(11)} "
            f"{m.group(12).strip()}"
        )

# any 065054 in raw log
idxs = [i for i, ln in enumerate(text.splitlines()) if "065054" in ln]
print("065054 lines", len(idxs))
lines = text.splitlines()
for i in idxs[:15]:
    print(i, lines[i][:160])
