"""Boot SIMH, capture words at VA 006350-006370 and R5 around PC=006356."""
from __future__ import annotations

import re
import subprocess
import threading
time = __import__("time")
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SIM_DIR = Path(r"C:\Users\deang\OneDrive\emulators\pdp11\rsx11mplus")
EXE = Path(r"C:\Users\deang\iCloudDrive\OneDrive\emulators\pdp11\rsts7\PDP11.exe")
SRC_INI = REPO / "tools" / "sim-ex-6356.ini"
INI = SIM_DIR / "sim-ex-6356.ini"
LOG = SIM_DIR / "becnt-simh-6356.log"
REPO_CAP = REPO / "build" / "captures" / "becnt-simh-6356.txt"
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
    saw_6356 = 0
    saw_5704 = 0
    saw_startup = False
    late_acthd = False

    while time.time() - t0 < MAX_SECS:
        if proc.poll() is not None:
            stop_reason = f"exit:{proc.returncode}"
            break
        if LOG.exists():
            text = LOG.read_text(encoding="utf-8", errors="replace")
            saw_6356 = text.count("==== E@006356 before ====")
            saw_5704 = text.count("==== E@005704 ACTHD-install ====")
            if "STARTUP" in text:
                saw_startup = True
            # late ACTHD around 060350 / 060320
            for m in re.finditer(r"==== W@ACTHD ====.*?1032:\s*([0-7]+)", text, re.S):
                v = int(m.group(1), 8)
                if v >= 0o060000:
                    late_acthd = True
            # Enough samples: several 6356 hits after maps, or past STARTUP with dump
            if saw_6356 >= 3 and saw_5704 >= 1:
                stop_reason = f"samples-6356x{saw_6356}-5704x{saw_5704}"
                break
            if saw_startup and saw_6356 >= 1 and (time.time() - t0) > 90:
                stop_reason = "past-STARTUP-with-6356"
                break
            if late_acthd and saw_6356 >= 1 and (time.time() - t0) > 60:
                stop_reason = "late-ACTHD-with-6356"
                break
        time.sleep(0.4)

    # Final interactive examine in case breaks were quiet
    try:
        proc.stdin.write("\x05")
        time.sleep(0.3)
        for cmd in (
            "ex pc\n",
            "ex r0-r5\n",
            "ex -v 001032\n",
            "ex 6350-6370\n",
            "ex -v 6356\n",
            "ex -v 6350/20\n",
            "quit\n",
        ):
            proc.stdin.write(cmd)
            proc.stdin.flush()
            time.sleep(0.15)
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
    print(f"counts: E@005704={saw_5704} E@006356={saw_6356} STARTUP={saw_startup}", flush=True)

    # Extract word dumps
    for label in ("E@005704 ACTHD-install", "E@006356 before", "E@006360 after-6356", "W@ACTHD"):
        c = text.count(f"==== {label} ====")
        print(f"  marker {label}: {c}", flush=True)

    # Parse first good 6350-6370 dump near 006356
    blocks = list(
        re.finditer(
            r"==== E@006356 before ====\n(.*?)(?=====\s|==== E@|\Z)",
            text,
            re.S,
        )
    )
    print(f"parsed E@006356 blocks={len(blocks)}", flush=True)
    for i, b in enumerate(blocks[:5], 1):
        chunk = b.group(1)
        r5m = re.search(r"^R5:\s*([0-7]+)", chunk, re.M)
        act = re.search(r"^1032:\s*([0-7]+)", chunk, re.M)
        print(f"  #{i} R5={r5m.group(1) if r5m else '?'} ACTHD={act.group(1) if act else '?'}", flush=True)
        # print examine lines
        for line in chunk.splitlines():
            if re.search(r"635[0-7]|636[0-7]|637[0-7]|6356:", line) or ":" in line and re.match(r"^[0-7]+:", line.strip()):
                if any(x in line for x in ("635", "636", "637", "R0", "R1", "R2", "R3", "R4", "R5", "PC", "PSW", "1032", "1060")):
                    print(f"    {line}", flush=True)

    # After blocks: R5 cleared?
    after = list(
        re.finditer(
            r"==== E@006360 after-6356 ====\n(.*?)(?=====\s|==== E@|\Z)",
            text,
            re.S,
        )
    )
    r5_cleared = False
    for i, b in enumerate(after[:10], 1):
        chunk = b.group(1)
        r5m = re.search(r"^R5:\s*([0-7]+)", chunk, re.M)
        if r5m and r5m.group(1) in ("0", "000000"):
            r5_cleared = True
            print(f"  AFTER#{i} R5=0 (CLEARED)", flush=True)
        elif r5m and i <= 5:
            print(f"  AFTER#{i} R5={r5m.group(1)}", flush=True)
    print(f"SIMH_R5_CLEARED_AT_6360={r5_cleared}", flush=True)

    # Dump any 6350-6370 style lines from whole log
    print("--- all 635x examine lines ---", flush=True)
    for line in text.splitlines():
        if re.search(r"\b635[0-7]:|\b636[0-7]:|\b637[0-7]:", line):
            print(line, flush=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
