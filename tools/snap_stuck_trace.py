#!/usr/bin/env python3
"""Identify softc @077122 and capture short I/O + clock trace."""

from __future__ import annotations

import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from benchmark_boot_times import (  # noqa: E402
    TelnetConnection,
    BenchmarkError,
    SHELL_PROMPT_RE,
    SHELL_BANNER,
)

HOST = "192.168.7.144"


def drain(conn: TelnetConnection, seconds: float) -> bytes:
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


def ensure_shell(conn: TelnetConnection) -> None:
    for seq in (b"\x1b>>", b">\r", b"\r"):
        conn.send(seq)
        data = drain(conn, 0.7)
        if SHELL_PROMPT_RE.search(data) or SHELL_BANNER in data:
            conn.send(b"\r")
            drain(conn, 0.3)
            return
    raise RuntimeError("no shell")


def sh(conn: TelnetConnection, cmd: str, wait: float = 1.5) -> bytes:
    conn.send(cmd.encode("ascii") + b"\r")
    return drain(conn, wait)


def mon(conn: TelnetConnection, cmd: str, wait: float = 1.2) -> bytes:
    conn.send(cmd.encode("ascii") + b"\r")
    return drain(conn, wait)


def main() -> int:
    conn = TelnetConnection(HOST, 23, timeout=2.0)
    conn.connect()
    ensure_shell(conn)

    print("\n=== softc / name strings (physical) ===", flush=True)
    conn.send(b"monitor\r")
    drain(conn, 0.5)
    mon(conn, "P", 1.5)
    for a in (
        "D077020",
        "D077000",
        "D077122:077200",
        "D047434",
        "D026516",
        "D054070",
        "D000000:000100",  # vectors / conf handlers
        "D002000",  # vector 200 area
    ):
        print(f"\n>>> {a}", flush=True)
        mon(conn, a, 1.2)

    # I-space code at probe helpers
    print("\n=== I-space probe helpers ===", flush=True)
    for a in ("M026516", "M026560", "M026700", "M054070", "M025500", "M0144640"):
        print(f"\n>>> {a}", flush=True)
        mon(conn, a, 1.0)

    mon(conn, "C", 0.3)
    mon(conn, ">", 0.3)

    print("\n=== enable traces briefly ===", flush=True)
    sh(conn, "set pcping=0", 1.0)
    sh(conn, "set io_trace=50", 1.0)
    sh(conn, "set clock_trace=20", 1.0)
    sh(conn, "set dl_trace=20", 1.0)
    print("\n=== exit to console 8s ===", flush=True)
    sh(conn, "exit", 0.5)
    drain(conn, 8.0)

    # back to shell, disable traces
    ensure_shell(conn)
    sh(conn, "set io_trace=0", 1.0)
    sh(conn, "set clock_trace=0", 1.0)
    sh(conn, "set dl_trace=0", 1.0)
    sh(conn, "set pcping=1", 1.0)
    sh(conn, "tty", 1.5)
    sh(conn, "lights", 1.0)
    sh(conn, "rp regs", 2.0)
    sh(conn, "exit", 0.5)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
