"""Run SIMH post-install watch; stop if R5=0 at @007042 or after STARTUP+settle."""
from __future__ import annotations

import re
import subprocess
import threading
import time
from pathlib import Path

SIM_DIR = Path(r"C:\Users\deang\OneDrive\emulators\pdp11\rsx11mplus")
EXE = SIM_DIR / "PDP11.exe"
INI = SIM_DIR / "sim-becnt-watch-post.ini"
LOG = SIM_DIR / "becnt-simh-post.log"
REPO_CAP = (
    Path(__file__).resolve().parents[1]
    / "build"
    / "captures"
    / "becnt-simh-post.txt"
)
MAX_SECS = 150


def main() -> int:
    src = Path(__file__).resolve().parents[1] / "tools" / "sim-becnt-watch-post.ini"
    INI.write_text(src.read_text(encoding="utf-8"), encoding="utf-8")
    if LOG.exists():
        LOG.unlink()

    proc = subprocess.Popen(
        [str(EXE), str(INI.name)],
        cwd=str(SIM_DIR),
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    assert proc.stdin and proc.stdout

    def drain() -> None:
        for line in proc.stdout:
            print(line, end="", flush=True)

    threading.Thread(target=drain, daemon=True).start()
    time.sleep(0.8)
    proc.stdin.write("boot rl0\n")
    proc.stdin.flush()

    stop_reason = "timeout"
    t0 = time.time()
    saw_startup = False
    while time.time() - t0 < MAX_SECS:
        if proc.poll() is not None:
            stop_reason = f"exit:{proc.returncode}"
            break
        if LOG.exists():
            text = LOG.read_text(encoding="utf-8", errors="replace")
            if "STARTUP" in text:
                saw_startup = True
            for m in re.finditer(
                r"==== E@007042 BMI/finish-sw ====.*?R5:\s*([0-7]+)",
                text,
                re.S,
            ):
                if m.group(1) in ("0", "000000"):
                    stop_reason = "R5zero@007042"
                    break
            if stop_reason == "R5zero@007042":
                break
            if saw_startup and (time.time() - t0) > 100:
                stop_reason = "past-STARTUP-ok"
                break
        time.sleep(0.5)

    if stop_reason == "R5zero@007042":
        try:
            proc.stdin.write("\x05")
            proc.stdin.write("ex -v 001032\n")
            proc.stdin.write("ex -v 001060\n")
            proc.stdin.write("ex -v 060320/40\n")
            proc.stdin.flush()
            time.sleep(1.0)
        except Exception:
            pass

    try:
        proc.stdin.write("\x05")
        proc.stdin.write("quit\n")
        proc.stdin.flush()
    except Exception:
        pass
    try:
        proc.wait(timeout=15)
    except subprocess.TimeoutExpired:
        proc.kill()

    text = LOG.read_text(encoding="utf-8", errors="replace") if LOG.exists() else ""
    REPO_CAP.parent.mkdir(parents=True, exist_ok=True)
    REPO_CAP.write_text(text, encoding="utf-8", errors="replace")
    print(f"\nSTOP={stop_reason} bytes={len(text)} -> {REPO_CAP}", flush=True)

    hits_7042 = len(re.findall(r"==== E@007042 BMI/finish-sw ====", text))
    r5_at = re.findall(
        r"==== E@007042 BMI/finish-sw ====.*?R5:\s*([0-7]+).*?1032:\s*([0-7]+)",
        text,
        re.S,
    )
    print(f"E@007042 hits={hits_7042} parsed={len(r5_at)}", flush=True)
    for i, (r5, acthd) in enumerate(r5_at[:30], 1):
        mark = " <--- R5zero" if r5 in ("0", "000000") else ""
        print(f"  #{i} R5={r5} $ACTHD={acthd}{mark}", flush=True)

    acthd_writes = re.findall(
        r"==== W@ACTHD ====.*?1032:\s*([0-7]+)",
        text,
        re.S,
    )
    print(f"W@ACTHD count={len(acthd_writes)} unique={sorted(set(acthd_writes))}", flush=True)
    for line in text.splitlines():
        if "STARTUP" in line or "SYSTEM FAULT" in line or "RED DL" in line:
            print(line, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
