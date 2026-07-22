from pathlib import Path
import re
from collections import Counter

log = Path(r"C:\Users\deang\OneDrive\emulators\pdp11\rsx11mplus\becnt-simh-post.log")
text = log.read_text(encoding="utf-8", errors="replace")
out = Path(__file__).resolve().parents[1] / "build" / "captures"
out.mkdir(parents=True, exist_ok=True)
(out / "becnt-simh-post.txt").write_text(text, encoding="utf-8", errors="replace")

print("bytes", len(text), "lines", text.count("\n") + 1)
for m in (
    "==== E@006464",
    "==== E@007506",
    "==== E@007042",
    "==== E@007040",
    "==== E@004750",
    "==== E@005536",
    "==== W@ACTHD",
    "STARTUP",
    "SYSTEM FAULT",
):
    # count runtime banners only (exclude "E; echo ====")
    c = sum(1 for line in text.splitlines() if line.startswith(m) or line == m)
    print(f"{c:6d}  {m}")

print("060320", len(re.findall(r"(?<![0-7])060320(?![0-7])", text)))
print("060350", len(re.findall(r"(?<![0-7])060350(?![0-7])", text)))

rt = list(
    re.finditer(
        r"==== E@007042 BMI/finish-sw ====\n.*?^R5:\s*([0-7]+)\n.*?^1032:\s*([0-7]+)",
        text,
        re.S | re.M,
    )
)
print("runtime_007042", len(rt))
for i, m in enumerate(rt[:15], 1):
    print(f"  #{i} R5={m.group(1)} ACTHD={m.group(2)}")

acts = []
for m in re.finditer(
    r"==== W@ACTHD ====\n.*?^R5:\s*([0-7]+)\n.*?^1032:\s*([0-7]+)",
    text,
    re.S | re.M,
):
    acts.append((m.group(2), m.group(1)))
print("W@ACTHD blocks", len(acts))
print("ACTHD hist", Counter(a for a, _ in acts).most_common(15))
print("W R5zero", sum(1 for _, r in acts if r in ("0", "000000")))

m = re.search(
    r"==== E@007506 post-003224 ====\n.*?^R0:\s*([0-7]+)\n.*?^R1:\s*([0-7]+)\n"
    r".*?^R2:\s*([0-7]+)\n.*?^R3:\s*([0-7]+)\n.*?^R4:\s*([0-7]+)\n"
    r".*?^R5:\s*([0-7]+)\n.*?^SP:\s*([0-7]+)\n.*?^1032:\s*([0-7]+)\n"
    r".*?^1060:\s*([0-7]+)",
    text,
    re.S | re.M,
)
if m:
    print(
        "first7506 R0-R5",
        m.group(1),
        m.group(2),
        m.group(3),
        m.group(4),
        m.group(5),
        m.group(6),
        "SP",
        m.group(7),
        "ACTHD",
        m.group(8),
        "TKTCB",
        m.group(9),
    )

peal0 = []
for m in re.finditer(
    r"==== E@004750 peal ====\n.*?^R5:\s*([0-7]+)\n.*?^1032:\s*([0-7]+)",
    text,
    re.S | re.M,
):
    if m.group(1) in ("0", "000000"):
        peal0.append(m.group(2))
print("peal R5=0", len(peal0), Counter(peal0).most_common(8))

# 007040: count how often R5 is 0
bpl = []
for m in re.finditer(
    r"==== E@007040 BPL/BMI ====\n.*?^R5:\s*([0-7]+)\n.*?^1032:\s*([0-7]+)",
    text,
    re.S | re.M,
):
    bpl.append((m.group(1), m.group(2)))
print("E@007040", len(bpl), "R5zero", sum(1 for r, _ in bpl if r in ("0", "000000")))
print("007040 ACTHD", Counter(a for _, a in bpl).most_common(10))

# STARTUP context
for line in text.splitlines():
    if "STARTUP" in line:
        print("STARTUP_LINE", repr(line[:120]))
