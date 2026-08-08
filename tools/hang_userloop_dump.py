#!/usr/bin/env python3
"""Dump user busy-loop at 050020 and step it."""

from __future__ import annotations

import sys
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from benchmark_boot_times import TelnetConnection, BenchmarkError, enter_shell  # noqa: E402

HOST = "192.168.7.144"
OUT = ROOT / "boot-benchmark-results-211bsd-postphys"


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


def sh(conn, cmd, wait=2.5):
    print(f"\n>>> {cmd}", flush=True)
    conn.send(cmd.encode() + b"\r")
    return drain(conn, wait)


def main():
    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    log = OUT / f"{stamp}-userloop-telnet.log"
    buf = bytearray()

    conn = TelnetConnection(HOST, 23, timeout=2.0)
    conn.connect()
    conn.send(b"\r")
    data = drain(conn, 1.0)
    if b"monitor>" not in data:
        if b"vpdp:/>" not in data:
            conn.send(b"\x1b>>")
            data = drain(conn, 2.5)
        if b"vpdp:/>" in data or b"management" in data:
            mon(conn, "monitor", 2.0)

    buf.extend(mon(conn, "P", 2.0))
    buf.extend(mon(conn, "U", 2.5))
    buf.extend(mon(conn, "M047760:050060", 2.5))
    buf.extend(mon(conn, "M015400:015440", 2.0))
    buf.extend(mon(conn, "M100720:100770", 2.0))  # user stack frame

    print("\n=== step 20 from pause ===\n", flush=True)
    for i in range(20):
        data = mon(conn, "S", 0.35)
        buf.extend(data)

    buf.extend(mon(conn, ">", 0.5))
    buf.extend(sh(conn, "rl regs", 3.0))
    buf.extend(sh(conn, "clock", 2.0))
    buf.extend(sh(conn, "tty", 2.0))
    buf.extend(sh(conn, "lights", 2.0))

    log.write_bytes(buf)
    print(f"\nlog={log}", flush=True)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
