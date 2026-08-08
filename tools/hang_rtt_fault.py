#!/usr/bin/env python3
"""Break on RTT; dump return PC/PS from stack; step; catch fault."""

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


def main():
    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    log = OUT / f"{stamp}-rtt-fault-telnet.log"
    buf = bytearray()

    conn = TelnetConnection(HOST, 23, timeout=2.0)
    conn.connect()
    conn.send(b"\r")
    data = drain(conn, 1.0)
    if b"monitor>" in data:
        print("already monitor", flush=True)
    elif b"vpdp:/>" in data:
        mon(conn, "monitor", 2.0)
    else:
        conn.send(b"\x1b>>")
        data = drain(conn, 2.5)
        if b"monitor>" in data:
            pass
        else:
            enter_shell(conn, 10.0, True)
            mon(conn, "monitor", 2.0)

    # Board should still be hung from prior run
    buf.extend(mon(conn, "P", 2.0))
    buf.extend(mon(conn, "B004160", 0.5))  # RTT

    for i in range(5):
        print(f"\n===== RTT HIT {i+1} =====\n", flush=True)
        buf.extend(mon(conn, "C", 0.3))
        time.sleep(0.4)
        hit = drain(conn, 1.2)
        buf.extend(hit)
        buf.extend(mon(conn, "P", 1.5))
        # Dump 8 words at SP — RTT will pop PC then PS
        buf.extend(mon(conn, "D147570:147610", 1.5))
        # Also dump via whatever SP is (state line)
        buf.extend(mon(conn, "U", 2.0))
        print("\n--- step RTT ---", flush=True)
        buf.extend(mon(conn, "S", 0.5))  # execute RTT
        buf.extend(mon(conn, "P", 1.5))  # may already be in trap
        buf.extend(mon(conn, "U", 2.5))
        # If we landed in user, step a few until trap
        for s in range(5):
            buf.extend(mon(conn, "S", 0.35))

    buf.extend(mon(conn, "B clear", 0.4))
    # Try phys dump with 18-bit style addresses from PAR
    # page0 PAR=013105 → base 01310500; try D1326500 (7 digits)
    buf.extend(mon(conn, "D1326500:1326540", 2.0))
    buf.extend(mon(conn, "D01326500", 1.5))
    buf.extend(mon(conn, "D13265036", 1.5))

    log.write_bytes(buf)
    print(f"\nlog={log}", flush=True)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
