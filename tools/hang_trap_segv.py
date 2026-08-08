#!/usr/bin/env python3
"""Reboot to hang; break on trap vector; dump MMR/CPUERR/PS for SEGV source."""

from __future__ import annotations

import re
import sys
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
OUT = ROOT / "boot-benchmark-results-211bsd-postphys"
# From earlier vector dump: 000014: 004332 — trap/seg entry
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


def sh(conn, cmd, wait=2.0):
    print(f"\n>>> {cmd}", flush=True)
    conn.send(cmd.encode() + b"\r")
    return drain(conn, wait)


def mon(conn, cmd, wait=1.5):
    print(f"\n>>> mon {cmd}", flush=True)
    conn.send(cmd.encode() + b"\r")
    return drain(conn, wait)


def boot_to_hang(conn):
    start = time.monotonic()
    last_cr = 0.0
    cr_n = 0
    user_at = None
    last_out = start
    buf = bytearray()
    while time.monotonic() < start + 180:
        now = time.monotonic()
        if cr_n < 25 and now - start < 60 and b"2.11 BSD" not in buf and now - last_cr >= 2.0:
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
            continue
        if user_at and now - last_out > 20:
            print("\n*** hang ***\n", flush=True)
            return buf
    return buf


def main():
    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    log = OUT / f"{stamp}-trap-segv-telnet.log"
    buf = bytearray()

    conn = TelnetConnection(HOST, 23, timeout=2.0)
    conn.connect()
    enter_shell(conn, 10.0, True)
    shell_command(conn, "rm /pdpconfig.ini", 5.0, True)
    shell_command(conn, "cp /pdpconfig-211bsd.ini /pdpconfig.ini", 5.0, True)
    shell_command(conn, "set pcping=0", 5.0, True)
    shell_command(conn, "reset", 5.0, True)
    conn.send(b"exit\r")
    drain(conn, 1.0)

    buf.extend(boot_to_hang(conn))
    enter_shell(conn, 10.0, True)
    buf.extend(sh(conn, "monitor", 2.0))

    # Confirm still in issig loop, then catch traps
    buf.extend(mon(conn, "P", 1.5))
    buf.extend(mon(conn, "U", 2.0))
    buf.extend(mon(conn, "D077140:077146", 1.0))

    print(f"\n=== break on trap {TRAP}, collect 6 hits ===\n", flush=True)
    buf.extend(mon(conn, f"B{TRAP}", 0.5))
    for i in range(6):
        buf.extend(mon(conn, "C", 0.3))
        time.sleep(0.25)
        drain(conn, 0.5)
        data = mon(conn, "P", 1.5)
        buf.extend(data)
        buf.extend(mon(conn, "U", 2.0))
        # stacked PS/PC from trap - typically on kernel stack
        # Also dump R0-R5 from state line already in P output
        buf.extend(mon(conn, "D077140:077146", 1.0))
        print(f"--- trap hit {i+1} done ---", flush=True)

    buf.extend(mon(conn, "B clear", 0.4))
    log.write_bytes(buf)
    print(f"\nlog={log}", flush=True)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
