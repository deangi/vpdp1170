"""Reset ESP32 on COM18 and capture RSX boot console output."""
import serial
import time
from pathlib import Path

port = "COM18"
secs = 220
out = Path(__file__).resolve().parents[1] / "build" / "captures"
out.mkdir(parents=True, exist_ok=True)
path = out / "rsx-boot-56.txt"


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


def hard_reset(s):
    # ESP32-S3 USB-Serial/JTAG: pulse DTR/RTS to reset
    s.dtr = False
    s.rts = False
    time.sleep(0.05)
    s.dtr = True
    s.rts = True
    time.sleep(0.15)
    s.dtr = False
    s.rts = False


s = open_port()
if not s:
    raise SystemExit(2)

try:
    hard_reset(s)
    print("reset pulsed", flush=True)
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

# Keep lines quiet after reopen so we don't re-reset mid-boot
try:
    s.dtr = False
    s.rts = False
except Exception:
    pass

print("capturing", secs, "s ->", path, flush=True)
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
            s.dtr = False
            s.rts = False
        except Exception:
            pass

try:
    if s:
        s.close()
except Exception:
    pass

text = b"".join(buf).decode("utf-8", "replace")
path.write_text(text, encoding="utf-8", errors="replace")
print("bytes", len(text), flush=True)

keys = (
    "RSX-11",
    "RT-11",
    "508.KW",
    "MOU",
    "STARTUP",
    "Time and date",
    "Enter",
    "MCR",
    "SYSTEM FAULT",
    "Illegal",
    "LOGIN",
    ">",
    "MCR>",
)
shown = 0
for line in text.splitlines():
    if any(k in line for k in keys):
        print(line[:200])
        shown += 1
        if shown >= 80:
            print("...(truncated)...", flush=True)
            break
