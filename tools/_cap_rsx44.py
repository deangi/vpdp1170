import serial
import time
from pathlib import Path

port = "COM18"
out = Path(__file__).resolve().parents[1] / "build" / "captures"
out.mkdir(parents=True, exist_ok=True)
path = out / "rsx-watch-pcgated-44.txt"
secs = 150


def open_port():
    for i in range(40):
        try:
            s = serial.Serial(port, 115200, timeout=0.2)
            print("open", i, flush=True)
            return s
        except Exception as e:
            print("retry", i, type(e).__name__, e, flush=True)
            time.sleep(0.5)
    return None


s = open_port()
if not s:
    raise SystemExit(2)

try:
    s.dtr = False
    s.rts = False
    time.sleep(0.05)
    s.dtr = True
    s.rts = True
    time.sleep(0.15)
    s.dtr = False
    s.rts = False
except Exception as e:
    print("pulse err", e, flush=True)
try:
    s.close()
except Exception:
    pass
time.sleep(3)

s = open_port()
if not s:
    raise SystemExit(3)

print("capturing", secs, "s", flush=True)
t0 = time.time()
buf = []
while time.time() - t0 < secs:
    try:
        n = s.in_waiting
        if n:
            buf.append(s.read(n))
        else:
            time.sleep(0.05)
    except Exception as e:
        print("disconnect", type(e).__name__, e, flush=True)
        try:
            s.close()
        except Exception:
            pass
        time.sleep(1.5)
        s = open_port()
        if not s:
            print("reopen failed; abort", flush=True)
            break

try:
    if s:
        s.close()
except Exception:
    pass

text = b"".join(buf).decode("utf-8", "replace")
path.write_text(text, encoding="utf-8", errors="replace")
print("bytes", len(text), flush=True)
keys = (
    "R4nz-LIVE",
    "R4nz-step",
    "post-R4nz",
    "skip-CLR",
    "UNEXPECTED-CLR",
    "finish-sw",
    "switch-BPL",
    "switch-BMI",
    "SYSTEM FAULT",
    "RSX-11",
    "RED DL",
    "MOU",
    "HALT",
)
for line in text.splitlines():
    if any(k in line for k in keys):
        print(line)
