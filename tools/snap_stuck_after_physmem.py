#!/usr/bin/env python3
"""Snapshot stuck-after-physmem state: R4 device, LFC, RL CSRs, code at PC."""

from __future__ import annotations

import re
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


def mon(conn: TelnetConnection, cmd: str, wait: float = 1.0) -> bytes:
    conn.send(cmd.encode("ascii") + b"\r")
    return drain(conn, wait)


def main() -> int:
    conn = TelnetConnection(HOST, 23, timeout=2.0)
    conn.connect()
    ensure_shell(conn)
    conn.send(b"monitor\r")
    drain(conn, 0.5)
    mon(conn, "P", 1.5)
    print("\n>>> MMU / CPUERR", flush=True)
    mon(conn, "U", 2.5)
    print("\n>>> device softc @ R4=077122 (16 words)", flush=True)
    mon(conn, "M077122", 1.2)
    mon(conn, "M077142", 1.2)
    print("\n>>> RL CSRs 174400 and KW11-L 177546", flush=True)
    # These are I/O — M uses I-space; use physical via guest D-space page7
    # VA 177546 with KDSA7=177600 → I/O. M may not work; try anyway + D won't.
    mon(conn, "M174400", 1.0)
    mon(conn, "M177546", 1.0)
    mon(conn, "M177560", 1.0)  # console XCSR
    print("\n>>> code around recent PCs", flush=True)
    for a in (0o026700, 0o026760, 0o027200, 0o053300, 0o053640, 0o03100):
        mon(conn, f"M{a:06o}", 1.0)
    print("\n>>> stack frame", flush=True)
    mon(conn, "M147500", 1.2)
    mon(conn, "C", 0.3)
    mon(conn, ">", 0.3)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
