#!/usr/bin/env python3
"""While hung after user mem: catch trap @004332, dump fault PC/MMU."""

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


def mon(conn, cmd, wait=1.5):
    print(f"\n>>> {cmd}", flush=True)
    conn.send(cmd.encode() + b"\r")
    return drain(conn, wait)


def main():
    conn = TelnetConnection(HOST, 23, timeout=2.0)
    conn.connect()
    conn.send(b"\r")
    d = drain(conn, 1.0)
    if b"monitor>" not in d:
        if b"vpdp:" in d:
            mon(conn, "monitor", 1.5)
        else:
            conn.send(b"\x1b>>")
            d = drain(conn, 2.5)
            if b"vpdp:" in d or b"management" in d:
                mon(conn, "monitor", 1.5)

    # Catch several traps; at 004332 dump U + kernel stack frame pointing to fault
    mon(conn, "B004332", 0.4)
    for i in range(5):
        print(f"\n===== trap sample {i+1} =====\n", flush=True)
        mon(conn, "C", 0.2)
        time.sleep(0.8)
        hit = drain(conn, 1.2)
        if b"004332" not in hit and b"stopped" not in hit:
            mon(conn, "P", 1.0)
        mon(conn, "P", 1.5)
        mon(conn, "U", 3.0)
        # kernel stack around SP — trap frame often has saved PC
        mon(conn, "M147560:147620", 2.0)
        mon(conn, "D077140:077150", 1.5)  # p_sig area for proc 077122
    mon(conn, "B clear", 0.3)

    # Also break at RTT return to see user PC being restored
    print("\n===== RTT samples =====\n", flush=True)
    mon(conn, "B004160", 0.4)
    for i in range(4):
        print(f"\n--- RTT {i+1} ---\n", flush=True)
        mon(conn, "C", 0.2)
        time.sleep(0.5)
        drain(conn, 1.0)
        mon(conn, "P", 1.5)
        mon(conn, "U", 2.5)
        mon(conn, "M147560:147610", 2.0)
        # step RTT
        mon(conn, "S", 0.6)
        mon(conn, "P", 1.2)
        mon(conn, "U", 2.0)
    mon(conn, "B clear", 0.3)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
