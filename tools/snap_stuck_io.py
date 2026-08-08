#!/usr/bin/env python3
"""Dump physical I/O + softc while paused after phys mem hang."""

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


def mon(conn: TelnetConnection, cmd: str, wait: float = 1.2) -> bytes:
    conn.send(cmd.encode("ascii") + b"\r")
    return drain(conn, wait)


def sh(conn: TelnetConnection, cmd: str, wait: float = 1.5) -> bytes:
    conn.send(cmd.encode("ascii") + b"\r")
    return drain(conn, wait)


def main() -> int:
    conn = TelnetConnection(HOST, 23, timeout=2.0)
    conn.connect()
    ensure_shell(conn)

    print("\n=== shell status / rp / clock ===", flush=True)
    sh(conn, "help", 1.0)
    for cmd in ("rp status", "set", "lights"):
        print(f"\n>>> {cmd}", flush=True)
        sh(conn, cmd, 2.0)

    conn.send(b"monitor\r")
    drain(conn, 0.5)
    mon(conn, "P", 1.5)
    print("\n=== physical dumps (D) ===", flush=True)
    for a in (
        "D077122",
        "D077162",
        "D174400",  # may be wrong if D is phys RAM only
        "D177546",
        "D177560",
        "D026530",
        "D026560",
        "D026700",
        "D053640",
        "D0144640",
        "D147460",
    ):
        print(f"\n>>> {a}", flush=True)
        mon(conn, a, 1.0)

    print("\n=== RH70 peek (I) ===", flush=True)
    mon(conn, "I", 2.0)
    print("\n=== MMU ===", flush=True)
    mon(conn, "U", 2.5)

    # Try mapping I/O via D-space: KDSA7 should make 174400 work with M if D-space
    # Check whether M uses I or D — dump same via depositing nothing
    print("\n=== M softc + nearby dtab ===", flush=True)
    mon(conn, "M077100", 1.0)
    mon(conn, "M077120", 1.0)
    mon(conn, "M026532", 1.0)
    mon(conn, "M025500", 1.0)

    mon(conn, "C", 0.3)
    mon(conn, ">", 0.3)
    sh(conn, "exit", 1.0)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
