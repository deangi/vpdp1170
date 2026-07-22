"""Run SIMH @000034 finish-sw watch; stop after STARTUP settle or R5=0 @007034."""
from __future__ import annotations

import re
import subprocess
import threading
import time
from pathlib import Path

SIM_DIR = Path(r"C:\Users\deang\OneDrive\emulators\pdp11\rsx11mplus")
# PDP11.exe is not in rsx11mplus on this machine; rsts7 copy is the local binary.
EXE = Path(r"C:\Users\deang\OneDrive\emulators\pdp11\rsts7\PDP11.exe")
INI = SIM_DIR / "sim-becnt-watch-000034.ini"
LOG = SIM_DIR / "becnt-simh-000034.log"
REPO = Path(__file__).resolve().parents[1]
SRC_INI = REPO / "tools" / "sim-becnt-watch-000034.ini"
REPO_CAP = REPO / "build" / "captures" / "becnt-simh-000034.txt"
MAX_SECS = 180


def main() -> int:
    INI.write_text(SRC_INI.read_text(encoding="utf-8"), encoding="utf-8")
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
    time.sleep(1.0)
    proc.stdin.write("boot rl0\n")
    proc.stdin.flush()

    stop_reason = "timeout"
    t0 = time.time()
    saw_startup = False
    saw_time = False
    r5zero_hit = False

    while time.time() - t0 < MAX_SECS:
        if proc.poll() is not None:
            stop_reason = f"exit:{proc.returncode}"
            break
        if LOG.exists():
            text = LOG.read_text(encoding="utf-8", errors="replace")
            if "STARTUP" in text:
                saw_startup = True
            if "Time and date" in text or "Enter" in text and "time" in text.lower():
                saw_time = True
            # R5=0 at TSTB site
            for m in re.finditer(
                r"==== E@007034 TSTB ====.*?^R5:\s*([0-7]+).*?^0*34:\s*([0-7]+)",
                text,
                re.S | re.M,
            ):
                if m.group(1) in ("0", "000000"):
                    r5zero_hit = True
                    stop_reason = f"R5zero@007034 val={m.group(2)}"
                    break
            if r5zero_hit:
                break
            # Also catch R5=0 at decision
            if not r5zero_hit:
                for m in re.finditer(
                    r"==== E@007040 BPL/BMI ====.*?^R5:\s*([0-7]+).*?^0*34:\s*([0-7]+)",
                    text,
                    re.S | re.M,
                ):
                    if m.group(1) in ("0", "000000"):
                        r5zero_hit = True
                        stop_reason = f"R5zero@007040 val={m.group(2)}"
                        break
            if r5zero_hit:
                break
            # Healthy boot past Time/date + settle
            if saw_startup and saw_time and (time.time() - t0) > 120:
                stop_reason = "past-Time-date-ok"
                break
            if saw_startup and (time.time() - t0) > 150:
                stop_reason = "past-STARTUP-ok"
                break
        time.sleep(0.4)

    try:
        proc.stdin.write("\x05")
        proc.stdin.write("ex -v 000034\n")
        proc.stdin.write("ex -v 001032\n")
        proc.stdin.write("ex -v 001060\n")
        proc.stdin.write("ex psw\n")
        proc.stdin.flush()
        time.sleep(0.8)
        proc.stdin.write("quit\n")
        proc.stdin.flush()
    except Exception:
        pass
    try:
        proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        proc.kill()

    text = LOG.read_text(encoding="utf-8", errors="replace") if LOG.exists() else ""
    REPO_CAP.parent.mkdir(parents=True, exist_ok=True)
    REPO_CAP.write_text(text, encoding="utf-8", errors="replace")
    print(f"\nSTOP={stop_reason} bytes={len(text)} -> {REPO_CAP}", flush=True)

    # Summarize @007034 hits with R5 and @000034
    hits = list(
        re.finditer(
            r"==== E@007034 TSTB ====\n.*?^R5:\s*([0-7]+)\n"
            r".*?^PSW:\s*([0-7]+)\n.*?^0*34:\s*([0-7]+)\n"
            r".*?^1032:\s*([0-7]+)\n.*?^1060:\s*([0-7]+)",
            text,
            re.S | re.M,
        )
    )
    # Fallback if PSW order differs
    if not hits:
        hits = list(
            re.finditer(
                r"==== E@007034 TSTB ====\n.*?^R5:\s*([0-7]+)\n"
                r".*?^0*34:\s*([0-7]+)\n.*?^1032:\s*([0-7]+)",
                text,
                re.S | re.M,
            )
        )
        print(f"E@007034 hits={len(hits)} (loose parse)", flush=True)
        for i, m in enumerate(hits, 1):
            r5, val, act = m.group(1), m.group(2), m.group(3)
            mark = " <--- R5zero" if r5 in ("0", "000000") else ""
            lo = int(val, 8) & 0xFF
            print(
                f"  #{i} R5={r5} @000034={val} low={lo:03o} ACTHD={act}{mark}",
                flush=True,
            )
    else:
        print(f"E@007034 hits={len(hits)}", flush=True)
        for i, m in enumerate(hits, 1):
            r5, psw, val, act, tkt = m.groups()
            mark = " <--- R5zero" if r5 in ("0", "000000") else ""
            lo = int(val, 8) & 0xFF
            print(
                f"  #{i} R5={r5} PSW={psw} @000034={val} low={lo:03o} "
                f"ACTHD={act} TKTCB={tkt}{mark}",
                flush=True,
            )

    for label in (
        "==== E@007042 BMI ====",
        "==== E@007054 BPL ====",
        "==== E@007040 BPL/BMI ====",
        "STARTUP",
        "SYSTEM FAULT",
        "Time and date",
    ):
        c = sum(1 for line in text.splitlines() if label in line and not line.startswith("break"))
        print(f"  count {label!r}: {c}", flush=True)

    # R5=0 at 007040 with branch outcome (next BMI or BPL marker)
    for m in re.finditer(
        r"==== E@007040 BPL/BMI ====\n.*?^R5:\s*([0-7]+)\n.*?^PSW:\s*([0-7]+)\n"
        r".*?^0*34:\s*([0-7]+)",
        text,
        re.S | re.M,
    ):
        if m.group(1) in ("0", "000000"):
            psw = int(m.group(2), 8)
            n = (psw >> 3) & 1
            z = (psw >> 2) & 1
            lo = int(m.group(3), 8) & 0xFF
            print(
                f"R5zero@007040 PSW={m.group(2)} N={n} Z={z} "
                f"@000034={m.group(3)} low={lo:03o}",
                flush=True,
            )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
