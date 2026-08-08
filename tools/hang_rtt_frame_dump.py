#!/usr/bin/env python3
"""Dump RTT stack frame + first-fault context while hung at 065054."""

from __future__ import annotations

import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from benchmark_boot_times import TelnetConnection, BenchmarkError  # noqa: E402

HOST = "192.168.7.144"


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


def mon(conn, cmd, wait=1.6):
    print(f"\n>>> {cmd}", flush=True)
    conn.send(cmd.encode() + b"\r")
    return drain(conn, wait)


def to_monitor(conn):
    conn.send(b"\r")
    d = drain(conn, 1.0)
    if b"monitor>" in d:
        return
    if b"vpdp:" in d:
        mon(conn, "monitor", 1.5)
        return
    conn.send(b"\x1b>>")
    d = drain(conn, 2.5)
    if b"monitor>" not in d:
        mon(conn, "monitor", 1.5)


def main():
    conn = TelnetConnection(HOST, 23, timeout=2.0)
    conn.connect()
    to_monitor(conn)

    # Catch RTT with stack dump before step
    mon(conn, "B004160", 0.4)
    for i in range(3):
        print(f"\n==== RTT frame {i+1} ====\n", flush=True)
        mon(conn, "C", 0.2)
        time.sleep(0.6)
        drain(conn, 1.0)
        mon(conn, "P", 1.2)
        # Kernel stack: RTT will pull PC/PS from here
        mon(conn, "M147550:147620", 2.0)
        mon(conn, "U", 2.5)
        mon(conn, "S", 0.6)  # execute RTT
        mon(conn, "P", 1.2)
        mon(conn, "U", 2.5)
        # What does monitor think is at fault VA via physical?
        mon(conn, "D01310500:01310540", 1.5)  # may fail high phys
    mon(conn, "B clear", 0.3)

    # Dump init text around old MODF site and nearby
    print("\n==== user I dump via M (mapped pages 0-1) ====\n", flush=True)
    mon(conn, "P", 1.0)
    for a in (
        "M031060:031140",
        "M064000:064100",
        "M065040:065100",
        "M030000:030100",
        "M016000:016100",
    ):
        mon(conn, a, 2.0)

    # Trap entry: dump saved registers / cdevsw-ish
    mon(conn, "B004332", 0.4)
    mon(conn, "C", 0.2)
    time.sleep(0.8)
    drain(conn, 1.0)
    mon(conn, "P", 1.2)
    mon(conn, "U", 2.5)
    mon(conn, "M147560:147620", 2.0)
    mon(conn, "B clear", 0.3)

    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
