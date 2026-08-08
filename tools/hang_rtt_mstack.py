#!/usr/bin/env python3
"""Break RTT; dump KERNEL stack via M (virtual); step; show PC/PS."""

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
    log = OUT / f"{stamp}-rtt-mstack-telnet.log"
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
    buf.extend(mon(conn, "B004160", 0.5))
    buf.extend(mon(conn, "C", 0.3))
    time.sleep(0.5)
    buf.extend(drain(conn, 1.5))
    buf.extend(mon(conn, "P", 1.5))

    print("\n=== virtual stack at SP (expect PC,PS) ===\n", flush=True)
    buf.extend(mon(conn, "M147570:147610", 2.0))
    buf.extend(mon(conn, "D01272570:01272610", 2.0))  # phys guess page6

    print("\n=== step RTT ===\n", flush=True)
    buf.extend(mon(conn, "S", 0.5))
    buf.extend(mon(conn, "P", 1.5))
    buf.extend(mon(conn, "U", 2.5))

    # If in user at 065054, try M065054 and M016360
    buf.extend(mon(conn, "M016360:016400", 2.0))
    buf.extend(mon(conn, "M065050:065070", 2.0))
    buf.extend(mon(conn, "M056350:056370", 2.0))

    buf.extend(mon(conn, "B clear", 0.4))
    log.write_bytes(buf)
    print(f"\nlog={log}", flush=True)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
