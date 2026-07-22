from pathlib import Path
import re
t = Path(r"C:\Users\deang\OneDrive\Arduino\ESP32\Freenove_ESP32_S3_Disp\vpdp1170\build\captures\becnt-simh-6356.txt").read_text(encoding="utf-8", errors="replace")
m = re.search(
    r"==== E@005704 ACTHD-install ====.*?6350:\s*([0-7]+).*?6352:\s*([0-7]+).*?6354:\s*([0-7]+).*?6356:\s*([0-7]+).*?6360:\s*([0-7]+).*?6362:\s*([0-7]+).*?6364:\s*([0-7]+).*?6366:\s*([0-7]+).*?6370:\s*([0-7]+)",
    t,
    re.S,
)
print("first_5704_words", m.groups() if m else None)
for i, b in enumerate(re.finditer(r"==== E@006356 before ====\n(.*?)(?=\n==== |\Z)", t, re.S), 1):
    c = b.group(1)
    r5 = re.search(r"^R5:\s*([0-7]+)", c, re.M)
    pc = re.search(r"^PC:\s*([0-7]+)", c, re.M)
    act = re.search(r"^1032:\s*([0-7]+)", c, re.M)
    w = re.search(r"6356:\s*([0-7]+)", c)
    print("BEFORE", i, "PC", pc.group(1) if pc else None, "R5", r5.group(1) if r5 else None, "ACTHD", act.group(1) if act else None, "w6356", w.group(1) if w else None)
    for line in c.splitlines()[:30]:
        if line.strip():
            print(" ", line)
    print("---")
for i, b in enumerate(re.finditer(r"==== E@006360 after-6356 ====\n(.*?)(?=\n==== |\Z)", t, re.S), 1):
    c = b.group(1)
    r5 = re.search(r"^R5:\s*([0-7]+)", c, re.M)
    pc = re.search(r"^PC:\s*([0-7]+)", c, re.M)
    act = re.search(r"^1032:\s*([0-7]+)", c, re.M)
    print("AFTER", i, "PC", pc.group(1) if pc else None, "R5", r5.group(1) if r5 else None, "ACTHD", act.group(1) if act else None)
    for line in c.splitlines()[:20]:
        if line.strip():
            print(" ", line)
    print("---")
acts = sorted(set(re.findall(r"==== W@ACTHD ====.*?1032:\s*([0-7]+)", t, re.S)))
print("unique_ACTHD", acts)
r5z = [m.group(0) for m in re.finditer(r"==== E@006360 after-6356 ====\n.*?^R5:\s*0+\s*$", t, re.S | re.M)]
print("r5zero_after_count", len(r5z))
r5z2 = [m.group(0) for m in re.finditer(r"==== E@006356 before ====\n.*?^R5:\s*0+\s*$", t, re.S | re.M)]
print("r5zero_before_count", len(r5z2))
