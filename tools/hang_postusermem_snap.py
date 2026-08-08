#!/usr/bin/env python3
"""Snapshot hang after 2.11BSD user mem banners."""

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


def mon(conn, cmd, wait=1.8):
    print(f"\n>>> {cmd}", flush=True)
    conn.send(cmd.encode() + b"\r")
    return drain(conn, wait)


def main() -> int:
    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    log = OUT / f"{stamp}-postusermem-snap.log"
    buf = bytearray()

    conn = TelnetConnection(HOST, 23, timeout=2.0)
    conn.connect()
    conn.send(b"\r")
    data = drain(conn, 1.0)
    buf.extend(data)

    if b"monitor>" not in data:
        if b"vpdp:" in data:
            buf.extend(mon(conn, "monitor", 2.0))
        else:
            conn.send(b"\x1b>>")
            data = drain(conn, 2.5)
            buf.extend(data)
            if b"monitor>" not in data:
                if b"vpdp:" not in data:
                    enter_shell(conn, 10.0, True)
                buf.extend(mon(conn, "monitor", 2.0))

    buf.extend(mon(conn, "P", 2.5))
    buf.extend(mon(conn, "U", 3.5))

    print("\n=== 12 PC samples ===\n", flush=True)
    for i in range(12):
        buf.extend(mon(conn, "C", 0.2))
        time.sleep(0.12)
        data = mon(conn, "P", 1.0)
        buf.extend(data)
        for line in data.decode("latin-1", errors="replace").splitlines():
            if "state: PC=" in line:
                print(f"s{i+1}: {line.strip()[:140]}", flush=True)

    # dump around last PC if we can parse it
    buf.extend(mon(conn, "M050000:050040", 2.0))
    buf.extend(mon(conn, "M047760:050060", 2.0))
    buf.extend(mon(conn, "M026500:026560", 2.0))
    buf.extend(mon(conn, "D077122:077160", 2.0))

    # trap rate
    print("\n=== B004332 2s ===\n", flush=True)
    buf.extend(mon(conn, "B004332", 0.5))
    buf.extend(mon(conn, "C", 0.2))
    time.sleep(2.0)
    hit = drain(conn, 1.5)
    buf.extend(hit)
    print("trap" if (b"004332" in hit and b"stopped" in hit) or b"BREAK" in hit else "no trap", flush=True)
    buf.extend(mon(conn, "P", 1.5))
    buf.extend(mon(conn, "B clear", 0.4))

    buf.extend(mon(conn, ">", 0.4))
    print("\n>>> rl regs", flush=True)
    conn.send(b"rl regs\r")
    buf.extend(drain(conn, 3.0))
    print("\n>>> clock", flush=True)
    conn.send(b"clock\r")
    buf.extend(drain(conn, 2.0))
    print("\n>>> tty", flush=True)
    conn.send(b"tty\r")
    buf.extend(drain(conn, 2.0))

    log.write_bytes(buf)
    print(f"\nlog={log}", flush=True)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
