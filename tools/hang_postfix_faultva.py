#!/usr/bin/env python3
"""Post-FP-fix hang: on trap, dump user PC/PS from stack + insn + MMR."""

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
            if b"configure system" in buf or b"login: " in buf:
                print("\n*** past hang ***\n", flush=True)
                return True
            continue
        if user_at and now - last_out > 22:
            print("\n*** hang ***\n", flush=True)
            return False
    return False


def main():
    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    log = OUT / f"{stamp}-postfix-faultva-telnet.log"
    buf = bytearray()

    conn = TelnetConnection(HOST, 23, timeout=2.0)
    conn.connect()
    conn.send(b"\r")
    data = drain(conn, 0.8)
    if b"monitor>" in data:
        conn.send(b">\r")
        drain(conn, 1.0)

    enter_shell(conn, 12.0, True)
    shell_command(conn, "rm /pdpconfig.ini", 5.0, True)
    shell_command(conn, "cp /pdpconfig-211bsd.ini /pdpconfig.ini", 5.0, True)
    shell_command(conn, "set pcping=0", 5.0, True)
    shell_command(conn, "reset", 5.0, True)
    conn.send(b"exit\r")
    drain(conn, 1.0)

    boot_to_hang(conn)
    enter_shell(conn, 12.0, True)
    conn.send(b"monitor\r")
    buf.extend(drain(conn, 2.0))

    buf.extend(mon(conn, "P", 2.0))
    buf.extend(mon(conn, "U", 2.5))
    buf.extend(mon(conn, "D077140:077150", 1.2))

    print("\n=== trap hits: dump frame + MFPI-ish via M after forcing user maps? ===\n", flush=True)
    buf.extend(mon(conn, "B004332", 0.5))

    for i in range(4):
        print(f"\n===== HIT {i+1} =====\n", flush=True)
        buf.extend(mon(conn, "C", 0.3))
        time.sleep(0.5)
        hit = drain(conn, 1.5)
        buf.extend(hit)
        buf.extend(mon(conn, "P", 1.2))
        # At trap entry SP~147574; frame often has saved regs then PC/PS.
        # Dump generous stack window + U.
        buf.extend(mon(conn, "U", 2.5))
        buf.extend(mon(conn, "D147540:147620", 2.0))
        # User addresses from prior trace: R0=016036 R1=056357 MMR2=065054
        # Try M with current (kernel) maps — may be wrong; also phys via PAR.
        # User I page0 PAR=013105 → phys = (013105<<6)+va
        # Dump likely user PC region physically:
        # 01310500+016000 = 01326500 area
        buf.extend(mon(conn, "D01326500:01326540", 2.0))
        buf.extend(mon(conn, "D01326540:01326600", 2.0))
        # Fault VA 065054 page3 — unmapped, but dump what PAR says
        buf.extend(mon(conn, "D077140:077146", 1.0))
        # Single-step a few into trap to see vector reason path
        for _ in range(6):
            buf.extend(mon(conn, "S", 0.3))

    buf.extend(mon(conn, "B clear", 0.4))
    log.write_bytes(buf)
    print(f"\nlog={log}", flush=True)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
