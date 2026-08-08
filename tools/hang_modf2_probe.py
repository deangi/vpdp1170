#!/usr/bin/env python3
"""Live hang probe after MODF fix: state, U, T1000, trap sample."""

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
)

HOST = "192.168.7.144"
COM = "COM18"
OUT = ROOT / "boot-benchmark-results-211bsd-postphys"
TRACE_N = 1000

PC_RE = re.compile(
    r"kek trace: PC=([0-7]+).*?ins=([0-7]+)\s+(\S+(?:\s+\S+){0,8})",
)


def drain(conn, seconds):
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


def mon(conn, cmd, wait=1.5):
    print(f"\n>>> {cmd}", flush=True)
    conn.send(cmd.encode() + b"\r")
    return drain(conn, wait)


def sh(conn, cmd, wait=2.0):
    print(f"\n>>> {cmd}", flush=True)
    conn.send(cmd.encode() + b"\r")
    return drain(conn, wait)


def serial_reader(stop, log_path):
    import serial

    ser = serial.Serial(COM, 115200, timeout=0.2)
    with log_path.open("wb") as f:
        while not stop.is_set():
            data = ser.read(8192)
            if data:
                f.write(data)
                f.flush()
    ser.close()


def to_monitor(conn):
    conn.send(b"\r")
    data = drain(conn, 1.0)
    if b"monitor>" in data:
        return
    if b"vpdp:/>" in data:
        mon(conn, "monitor", 2.0)
        return
    conn.send(b">\r")
    data = drain(conn, 1.0)
    if b"vpdp:/>" in data:
        mon(conn, "monitor", 2.0)
        return
    conn.send(b"\x1b>>")
    data = drain(conn, 2.5)
    if b"vpdp:/>" in data or b"management shell" in data:
        mon(conn, "monitor", 2.0)
        return
    if b"monitor>" not in data:
        enter_shell(conn, 10.0, True)
        mon(conn, "monitor", 2.0)


def main():
    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    ser_log = OUT / f"{stamp}-modf2-T{TRACE_N}-com18.log"
    tel_log = OUT / f"{stamp}-modf2-telnet.log"
    buf = bytearray()

    stop = threading.Event()
    thr = threading.Thread(target=serial_reader, args=(stop, ser_log), daemon=True)
    thr.start()
    time.sleep(0.3)

    conn = TelnetConnection(HOST, 23, timeout=2.0)
    for attempt in range(8):
        try:
            conn.connect()
            break
        except Exception as exc:
            print("telnet", attempt, exc, flush=True)
            time.sleep(2)
            conn = TelnetConnection(HOST, 23, timeout=2.0)
    else:
        raise RuntimeError("telnet down")

    to_monitor(conn)

    # If in monitor, leave for rl regs then back
    buf.extend(mon(conn, "P", 2.0))
    buf.extend(mon(conn, "U", 2.5))
    buf.extend(mon(conn, "D077122:077170", 2.0))
    buf.extend(mon(conn, ">", 0.5))
    buf.extend(sh(conn, "rl regs", 3.0))
    mon(conn, "monitor", 2.0)

    print(f"\n=== T {TRACE_N} ===\n", flush=True)
    buf.extend(mon(conn, f"T {TRACE_N}", 0.8))
    buf.extend(mon(conn, "C", 0.4))
    time.sleep(max(8.0, TRACE_N / 80.0))
    buf.extend(mon(conn, "P", 2.0))
    buf.extend(mon(conn, "T 0", 0.5))

    print("\n=== B004332 x4 + RTT peek ===\n", flush=True)
    buf.extend(mon(conn, "B004332", 0.5))
    for i in range(4):
        print(f"\n--- trap {i+1} ---", flush=True)
        buf.extend(mon(conn, "C", 0.3))
        time.sleep(0.5)
        buf.extend(drain(conn, 1.2))
        buf.extend(mon(conn, "P", 1.5))
        buf.extend(mon(conn, "U", 2.0))
    buf.extend(mon(conn, "B clear", 0.4))

    # Break RTT, dump virtual stack, step
    buf.extend(mon(conn, "B004160", 0.5))
    buf.extend(mon(conn, "C", 0.3))
    time.sleep(0.5)
    buf.extend(drain(conn, 1.2))
    buf.extend(mon(conn, "P", 1.5))
    buf.extend(mon(conn, "M147570:147610", 2.0))
    buf.extend(mon(conn, "S", 0.5))
    buf.extend(mon(conn, "P", 1.5))
    buf.extend(mon(conn, "U", 2.5))
    buf.extend(mon(conn, "B clear", 0.4))
    buf.extend(mon(conn, ">", 0.5))

    stop.set()
    thr.join(timeout=2.0)
    tel_log.write_bytes(buf)

    text = ser_log.read_text(encoding="latin-1", errors="replace")
    counts = collections.Counter()
    modf = 0
    for m in PC_RE.finditer(text):
        pc, ins, dis = m.groups()
        counts[f"{pc} {dis[:45]}"] += 1
        if "MODF" in dis:
            modf += 1
    print("\n=== hottest PCs ===", flush=True)
    for k, n in counts.most_common(20):
        print(f"  {n:4d}  {k}", flush=True)
    print(f"\nMODF trace lines={modf}", flush=True)
    print(f"serial={ser_log}\ntelnet={tel_log}", flush=True)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
