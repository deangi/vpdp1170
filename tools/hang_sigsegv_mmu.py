#!/usr/bin/env python3
"""Dump MMU / trap state while hung in init SIGSEGV loop."""

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
    log = OUT / f"{stamp}-sigsegv-mmu-telnet.log"
    buf = bytearray()

    conn = TelnetConnection(HOST, 23, timeout=2.0)
    conn.connect()
    conn.send(b"\r")
    data = drain(conn, 1.0)
    buf.extend(data)

    if b"monitor>" not in data:
        if b"vpdp:/>" not in data:
            conn.send(b"\x1b>>")
            data = drain(conn, 2.5)
            buf.extend(data)
        if b"vpdp:/>" in data or b"management shell" in data:
            buf.extend(mon(conn, "monitor", 2.0))

    for cmd in (
        "P",
        "U",
        "rl regs",
        "D077122:077220",  # full-ish proc
        # p_sigignore was +40; dump p_sigmask region and u-area hints
        "D0177576:0177600",  # guess — skip
        "M0177700:0177776",  # I/O page peek via M may fail
        "D177550:177570",  # MMR0/MMR1/MMR2/MMR3 / CPUERR area phys
        "D177560:177570",
        "D177500:177520",
        # trap vectors
        "D000014:000040",
        "D000250:000300",
        # recent stack frame at SP from last pause ~147520
        "D147450:147560",
        "M147450:147560",
    ):
        # rl regs is shell cmd — only if we bounce
        if cmd == "rl regs":
            buf.extend(mon(conn, ">", 0.5))
            print("\n>>> rl regs", flush=True)
            conn.send(b"rl regs\r")
            buf.extend(drain(conn, 2.0))
            conn.send(b"monitor\r")
            buf.extend(drain(conn, 1.5))
            continue
        buf.extend(mon(conn, cmd, 2.0))

    # Break on trap entry / segfault path if we can find it.
    # Single-step a few times recording PC when p_sig gets set.
    buf.extend(mon(conn, "B026532", 0.4))
    buf.extend(mon(conn, "C", 0.3))
    time.sleep(0.3)
    drain(conn, 0.5)
    buf.extend(mon(conn, "P", 1.2))
    # After clearing path, continue until something posts signal — watch 077142
    print("\n=== watch p_sig reconstitution ===\n", flush=True)
    buf.extend(mon(conn, "B clear", 0.3))
    # Zero p_sig manually and see if it returns
    buf.extend(mon(conn, "W077142=000000", 0.5))  # clear low p_sig
    buf.extend(mon(conn, "D077140:077146", 1.0))
    buf.extend(mon(conn, "C", 0.3))
    time.sleep(0.5)
    buf.extend(mon(conn, "P", 1.5))
    buf.extend(mon(conn, "D077140:077146", 1.0))
    buf.extend(mon(conn, "U", 2.0))

    log.write_bytes(buf)
    print(f"\nlog={log}", flush=True)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
