#!/usr/bin/env python3
"""Boot to hang; dump 0266xx caller; T 200 from break after ffs return."""

from __future__ import annotations

import re
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from benchmark_boot_times import (  # noqa: E402
    TelnetConnection,
    BenchmarkError,
    SHELL_PROMPT_RE,
    SHELL_BANNER,
)

HOST = "192.168.7.144"
COM = "COM18"
OUT = ROOT / "boot-benchmark-results-211bsd-postphys"
# Break just AFTER ffs returns: 026656 is first insn after JSR at 026652
BP_AFTER = "026656"
BP_JSR = "026652"


def drain(conn: TelnetConnection, seconds: float) -> bytes:
    deadline = time.monotonic() + seconds
    data = bytearray()
    while time.monotonic() < deadline:
        try:
            chunk = conn.receive()
        except BenchmarkError:
            break
        if chunk:
            data.extend(chunk)
            sys.stdout.write(chunk.decode("latin-1", errors="replace"))
            sys.stdout.flush()
    return bytes(data)


def ensure_shell(conn: TelnetConnection) -> None:
    for seq in (b"\x1b>>", b">\r", b"\r"):
        conn.send(seq)
        data = drain(conn, 1.0)
        if SHELL_PROMPT_RE.search(data) or SHELL_BANNER in data:
            conn.send(b"\r")
            drain(conn, 0.4)
            return
    raise RuntimeError("no shell")


def sh(conn: TelnetConnection, cmd: str, wait: float = 2.0) -> bytes:
    print(f"\n>>> {cmd}", flush=True)
    conn.send(cmd.encode("ascii") + b"\r")
    return drain(conn, wait)


def mon(conn: TelnetConnection, cmd: str, wait: float = 1.2) -> bytes:
    print(f"\n>>> mon {cmd}", flush=True)
    conn.send(cmd.encode("ascii") + b"\r")
    return drain(conn, wait)


def serial_reader(stop: threading.Event, log_path: Path) -> None:
    import serial

    ser = serial.Serial(COM, 115200, timeout=0.2)
    with log_path.open("wb") as f:
        while not stop.is_set():
            data = ser.read(8192)
            if data:
                f.write(data)
                f.flush()
    ser.close()


def boot_to_hang(conn: TelnetConnection) -> bytearray:
    buf = bytearray()
    for cmd, w in (
        ("rm /pdpconfig.ini", 3.0),
        ("cp /pdpconfig-211bsd.ini /pdpconfig.ini", 3.0),
        ("set pcping=0", 1.5),
    ):
        buf.extend(sh(conn, cmd, w))
    buf.extend(sh(conn, "reset", 1.0))
    time.sleep(0.3)
    buf.extend(sh(conn, "exit", 1.0))

    start = time.monotonic()
    last_out = start
    last_cr = 0.0
    cr_n = 0
    user_at = None
    while time.monotonic() < start + 200:
        now = time.monotonic()
        if cr_n < 20 and now - start < 50 and b"2.11 BSD" not in buf and now - last_cr >= 2.0:
            conn.send(b"\r")
            last_cr = now
            cr_n += 1
        try:
            chunk = conn.receive()
        except BenchmarkError:
            time.sleep(0.05)
            continue
        if chunk:
            buf.extend(chunk)
            last_out = now
            sys.stdout.write(chunk.decode("latin-1", errors="replace"))
            sys.stdout.flush()
            if b"user mem" in buf and user_at is None:
                user_at = now
                print("\n*** user mem ***\n", flush=True)
            if b"configure system" in buf or b"login: " in buf:
                break
            continue
        if user_at and now - last_out > 30:
            print("\n*** hang ***\n", flush=True)
            break
    return buf


def main() -> int:
    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    serial_log = OUT / f"{stamp}-caller-com18.log"
    telnet_log = OUT / f"{stamp}-caller-telnet.log"

    stop = threading.Event()
    thr = threading.Thread(target=serial_reader, args=(stop, serial_log), daemon=True)
    thr.start()
    time.sleep(0.4)

    conn = TelnetConnection(HOST, 23, timeout=2.0)
    for attempt in range(12):
        try:
            conn.connect()
            break
        except Exception as exc:
            print("telnet", attempt, exc, flush=True)
            time.sleep(2)
            conn = TelnetConnection(HOST, 23, timeout=2.0)
    else:
        raise RuntimeError("telnet down")

    ensure_shell(conn)
    buf = boot_to_hang(conn)
    ensure_shell(conn)

    print("=== dump caller region ===", flush=True)
    buf.extend(sh(conn, "rl regs", 2.0))
    buf.extend(sh(conn, "monitor", 0.5))
    buf.extend(mon(conn, "P", 2.0))
    for cmd, w in (
        ("M026500", 1.5),
        ("M026600", 1.5),
        ("M026640", 1.5),
        ("M026700", 1.5),
        ("M144616", 1.2),
        ("D077122", 1.2),
    ):
        buf.extend(mon(conn, cmd, w))

    # Break after JSR return; single-step a bit then T 150
    print(f"\n=== B{BP_AFTER}, step/trace past ffs ===\n", flush=True)
    buf.extend(mon(conn, f"B{BP_AFTER}", 0.5))
    buf.extend(mon(conn, "C", 0.3))

    # Wait for break
    deadline = time.monotonic() + 8.0
    saw = False
    while time.monotonic() < deadline:
        data = drain(conn, 0.4)
        buf.extend(data)
        if b"BREAK" in data or b"monitor pause" in data or b"PC=026656" in data:
            saw = True
            break
        # serial has BREAK; peek state
        time.sleep(0.2)
        if serial_log.exists() and f"pc={BP_AFTER}" in serial_log.read_text(
            encoding="latin-1", errors="replace"
        ):
            saw = True
            break

    print(f"break_seen={saw}", flush=True)
    # Dump state at return
    buf.extend(mon(conn, "P", 1.5))  # ensure paused
    # Single-step ~25 instructions with S (logs to serial via panic path? S may print state)
    for i in range(30):
        buf.extend(mon(conn, "S", 0.35))

    buf.extend(mon(conn, "B clear", 0.4))
    # Also grab a short free-running trace of the spin
    buf.extend(mon(conn, "T 200", 0.4))
    buf.extend(mon(conn, "C", 0.3))
    time.sleep(3.0)
    buf.extend(mon(conn, "P", 1.5))
    buf.extend(mon(conn, "T 0", 0.3))
    buf.extend(mon(conn, ">", 0.4))
    buf.extend(sh(conn, "rl regs", 2.0))
    buf.extend(sh(conn, "exit", 0.5))

    stop.set()
    thr.join(timeout=2)
    conn.close()
    telnet_log.write_bytes(buf)

    text = serial_log.read_text(encoding="latin-1", errors="replace")
    # Join wrapped lines
    raw = []
    cur = ""
    for ln in text.splitlines():
        if ln.startswith("[vpdp"):
            if cur:
                raw.append(cur)
            cur = ln
        else:
            cur += " " + ln.strip()
    if cur:
        raw.append(cur)

    print("\n=== post-ffs / step lines (sample) ===", flush=True)
    interesting = [
        ln
        for ln in raw
        if any(
            k in ln
            for k in (
                "BREAK",
                "HALT regs",
                "kek trace:",
                "monitor pause",
                "0266",
                "1446",
                "174400",
            )
        )
    ]
    for ln in interesting[:40]:
        print(ln[:220], flush=True)
    print("...", flush=True)
    for ln in interesting[-30:]:
        print(ln[:220], flush=True)

    # PC histogram from kek trace
    pcs = []
    for ln in raw:
        m = re.search(r"kek trace: PC=([0-7]+).*?\s([A-Z][A-Z0-9/ ].{0,40})$", ln)
        if m:
            pcs.append((m.group(1), m.group(2).strip()))
    if pcs:
        from collections import Counter

        print("\n=== T200 top PCs ===", flush=True)
        for pc, n in Counter(p for p, _ in pcs).most_common(15):
            sample = next(op for p, op in pcs if p == pc)
            print(f"  {pc} x{n}  {sample}", flush=True)

    print(f"\ntelnet={telnet_log}\ncom18={serial_log}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
