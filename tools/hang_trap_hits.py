#!/usr/bin/env python3
"""From current monitor session: break trap 004332, dump MMR on hits."""

from __future__ import annotations

import sys
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from benchmark_boot_times import TelnetConnection, BenchmarkError  # noqa: E402

HOST = "192.168.7.144"
OUT = ROOT / "boot-benchmark-results-211bsd-postphys"
TRAP = "004332"


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
    log = OUT / f"{stamp}-trap-hits-telnet.log"
    buf = bytearray()

    conn = TelnetConnection(HOST, 23, timeout=2.0)
    conn.connect()
    conn.send(b"\r")
    data = drain(conn, 1.0)
    buf.extend(data)

    if b"monitor>" not in data:
        if b"vpdp:/>" in data:
            buf.extend(mon(conn, "monitor", 2.0))
        else:
            conn.send(b">\r")
            data = drain(conn, 1.0)
            buf.extend(data)
            if b"vpdp:/>" in data:
                buf.extend(mon(conn, "monitor", 2.0))
            else:
                conn.send(b"\x1b>>")
                data = drain(conn, 2.5)
                buf.extend(data)
                buf.extend(mon(conn, "monitor", 2.0))

    buf.extend(mon(conn, "P", 1.5))
    buf.extend(mon(conn, "D077140:077146", 1.0))
    buf.extend(mon(conn, f"B{TRAP}", 0.5))

    for i in range(8):
        print(f"\n===== TRAP HIT {i+1} =====\n", flush=True)
        buf.extend(mon(conn, "C", 0.3))
        # Wait for break — may need longer if traps are rare
        time.sleep(0.8)
        hit = drain(conn, 1.5)
        buf.extend(hit)
        if b"BREAK" not in hit and b"004332" not in hit and b"stopped" not in hit:
            # force pause to see where we are
            buf.extend(mon(conn, "P", 1.5))
        else:
            # already stopped on break; P may no-op
            buf.extend(mon(conn, "\r", 0.5))
        buf.extend(mon(conn, "U", 2.0))
        buf.extend(mon(conn, "D077140:077146", 1.0))
        # kernel stack around SP
        buf.extend(mon(conn, "D147450:147560", 1.5))

    buf.extend(mon(conn, "B clear", 0.4))
    log.write_bytes(buf)
    print(f"\nlog={log}", flush=True)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
