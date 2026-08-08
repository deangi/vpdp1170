#!/usr/bin/env python3
"""Post-FP-fix: boot 211bsd to hang, dump state, T1000 on COM18, trap sample."""

from __future__ import annotations

import collections
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
    enter_shell,
    shell_command,
)

HOST = "192.168.7.144"
COM = "COM18"
OUT = ROOT / "boot-benchmark-results-211bsd-postphys"
TRACE_N = 1000

PC_RE = re.compile(
    r"PC=([0-7]+).*?ins=([0-7]+)\s+(\S+(?:\s+\S+){0,6}).*?R4=([0-7]+)",
    re.DOTALL,
)
BREAK_RE = re.compile(
    r"kek BREAK pc=([0-7]+).*?R0=([0-7]+) R1=([0-7]+).*?SP=([0-7]+)",
)


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


def sh(conn, cmd, wait=2.0) -> bytes:
    print(f"\n>>> {cmd}", flush=True)
    conn.send(cmd.encode() + b"\r")
    return drain(conn, wait)


def mon(conn, cmd, wait=1.5) -> bytes:
    print(f"\n>>> mon {cmd}", flush=True)
    conn.send(cmd.encode() + b"\r")
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
    start = time.monotonic()
    last_cr = 0.0
    cr_n = 0
    user_at = None
    last_out = start
    while time.monotonic() < start + 200:
        now = time.monotonic()
        if (
            cr_n < 25
            and now - start < 60
            and b"2.11 BSD" not in buf
            and now - last_cr >= 2.0
        ):
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
            if b"configure system" in buf or b"login: " in buf or b"\n# " in buf:
                print("\n*** progressed past hang point ***\n", flush=True)
                return buf
            continue
        if user_at and now - last_out > 25:
            print("\n*** hang ***\n", flush=True)
            return buf
    print("\n*** timeout ***\n", flush=True)
    return buf


def summarize_trace(text: str) -> None:
    counts: collections.Counter[str] = collections.Counter()
    samples = []
    for m in PC_RE.finditer(text):
        pc, ins, dis, r4 = m.groups()
        key = f"{pc} {dis.strip()[:40]}"
        counts[key] += 1
        if len(samples) < 12:
            samples.append((pc, dis.strip()[:50], r4))
    print("\n=== T1000 hottest PCs ===", flush=True)
    for key, n in counts.most_common(15):
        print(f"  {n:4d}  {key}", flush=True)
    print("\n=== first samples ===", flush=True)
    for pc, dis, r4 in samples:
        print(f"  PC={pc} R4={r4} {dis}", flush=True)
    modf = sum(1 for k, n in counts.items() if "MODF" in k)
    issig = sum(n for k, n in counts.items() if k.startswith("026") or k.startswith("1446"))
    trap = sum(n for k, n in counts.items() if k.startswith("004332") or k.startswith("004"))
    print(f"\nMODF-ish keys={modf}  026/1446(issig/ffs) hits~={issig}  004xx hits~={trap}", flush=True)


def main() -> int:
    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    ser_log = OUT / f"{stamp}-postfix-T{TRACE_N}-com18.log"
    tel_log = OUT / f"{stamp}-postfix-telnet.log"

    stop = threading.Event()
    thr = threading.Thread(target=serial_reader, args=(stop, ser_log), daemon=True)
    thr.start()
    time.sleep(0.4)

    conn = TelnetConnection(HOST, 23, timeout=2.0)
    for attempt in range(10):
        try:
            conn.connect()
            break
        except Exception as exc:
            print("telnet", attempt, exc, flush=True)
            time.sleep(2)
            conn = TelnetConnection(HOST, 23, timeout=2.0)
    else:
        raise RuntimeError("telnet down")

    # Leave monitor if stuck there
    conn.send(b"\r")
    data = drain(conn, 0.8)
    if b"monitor>" in data:
        conn.send(b">\r")
        drain(conn, 1.0)

    enter_shell(conn, 12.0, True)
    shell_command(conn, "rm /pdpconfig.ini", 5.0, True)
    shell_command(conn, "cp /pdpconfig-211bsd.ini /pdpconfig.ini", 5.0, True)
    shell_command(conn, "set pcping=0", 5.0, True)
    shell_command(conn, "reset", 5.0, True)
    conn.send(b"exit\r")
    drain(conn, 1.0)

    print("\n=== boot ===\n", flush=True)
    boot_buf = boot_to_hang(conn)
    tel_log.write_bytes(boot_buf)

    enter_shell(conn, 12.0, True)
    sh(conn, "rl regs", 3.0)
    sh(conn, "monitor", 2.0)
    mon(conn, "P", 2.0)
    mon(conn, "U", 2.5)
    mon(conn, "D077122:077170", 2.0)

    print(f"\n=== T {TRACE_N} ===\n", flush=True)
    mon(conn, f"T {TRACE_N}", 0.8)
    mon(conn, "C", 0.4)
    # wait for trace to finish on serial
    time.sleep(max(8.0, TRACE_N / 80.0))
    mon(conn, "P", 2.0)
    mon(conn, "T 0", 0.5)

    # Trap sample: still MODF?
    print("\n=== B004332 x5 ===\n", flush=True)
    mon(conn, "B004332", 0.5)
    for i in range(5):
        mon(conn, "C", 0.3)
        time.sleep(0.5)
        hit = drain(conn, 1.2)
        if b"BREAK" not in hit and b"004332" not in hit:
            mon(conn, "P", 1.5)
        mon(conn, "U", 2.0)
        print(f"--- trap {i+1} ---", flush=True)
    mon(conn, "B clear", 0.4)
    mon(conn, ">", 0.5)

    stop.set()
    thr.join(timeout=2.0)

    text = ser_log.read_text(encoding="latin-1", errors="replace")
    summarize_trace(text)
    print(f"\nserial={ser_log}\ntelnet={tel_log}", flush=True)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
