#!/usr/bin/env python3
"""Telnet-only hang probe (COM18 held by Arduino IDE)."""

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


def sh(conn, cmd, wait=2.5):
    print(f"\n>>> {cmd}", flush=True)
    conn.send(cmd.encode() + b"\r")
    return drain(conn, wait)


def main():
    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    log = OUT / f"{stamp}-telnet-only-probe.log"
    buf = bytearray()

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

    conn.send(b"\r")
    data = drain(conn, 1.0)
    buf.extend(data)
    if b"monitor>" in data:
        pass
    elif b"vpdp:/>" in data:
        buf.extend(mon(conn, "monitor", 2.0))
    else:
        conn.send(b"\x1b>>")
        data = drain(conn, 2.5)
        buf.extend(data)
        if b"monitor>" in data:
            pass
        elif b"vpdp:/>" in data or b"management" in data:
            buf.extend(mon(conn, "monitor", 2.0))
        else:
            enter_shell(conn, 10.0, True)
            buf.extend(mon(conn, "monitor", 2.0))

    buf.extend(mon(conn, "P", 2.0))
    buf.extend(mon(conn, "U", 2.5))
    buf.extend(mon(conn, "D077140:077150", 1.5))

    # Sample 10 random pauses
    print("\n=== 10 PC samples ===\n", flush=True)
    pcs = []
    for i in range(10):
        buf.extend(mon(conn, "C", 0.3))
        time.sleep(0.15)
        data = mon(conn, "P", 1.2)
        buf.extend(data)
        text = data.decode("latin-1", errors="replace")
        for line in text.splitlines():
            if "state: PC=" in line or "HALT regs" in line:
                print(f"sample{i+1}: {line.strip()[:120]}", flush=True)
                if "PC=" in line:
                    pcs.append(line)

    buf.extend(mon(conn, ">", 0.5))
    buf.extend(sh(conn, "rl regs", 3.0))
    buf.extend(mon(conn, "monitor", 2.0))

    # Trap frequency: break 004332, see if hits quickly
    print("\n=== trap 004332 wait ===\n", flush=True)
    buf.extend(mon(conn, "B004332", 0.5))
    buf.extend(mon(conn, "C", 0.3))
    time.sleep(2.0)
    hit = drain(conn, 2.0)
    buf.extend(hit)
    if b"BREAK" in hit or b"004332" in hit or b"stopped" in hit:
        print("TRAP HIT", flush=True)
        buf.extend(mon(conn, "P", 1.5))
        buf.extend(mon(conn, "U", 2.5))
        buf.extend(mon(conn, "D077140:077146", 1.2))
        # step to RTT path or dump
        buf.extend(mon(conn, "B004160", 0.5))
        buf.extend(mon(conn, "C", 0.3))
        time.sleep(1.0)
        buf.extend(drain(conn, 1.5))
        buf.extend(mon(conn, "P", 1.5))
        buf.extend(mon(conn, "M147570:147610", 2.0))
        buf.extend(mon(conn, "S", 0.5))
        buf.extend(mon(conn, "P", 1.5))
        buf.extend(mon(conn, "U", 2.5))
    else:
        print("no trap in 2s — force P", flush=True)
        buf.extend(mon(conn, "P", 1.5))
        buf.extend(mon(conn, "U", 2.5))

    buf.extend(mon(conn, "B clear", 0.4))
    # Also try issignal loop
    print("\n=== B026532 ===\n", flush=True)
    buf.extend(mon(conn, "B026532", 0.5))
    buf.extend(mon(conn, "C", 0.3))
    time.sleep(1.5)
    hit2 = drain(conn, 1.5)
    buf.extend(hit2)
    print("issig" if (b"026532" in hit2 or b"BREAK" in hit2) else "no issig hit", flush=True)
    buf.extend(mon(conn, "P", 1.5))
    buf.extend(mon(conn, "B clear", 0.4))

    log.write_bytes(buf)
    print(f"\nlog={log}", flush=True)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
