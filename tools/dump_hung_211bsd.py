#!/usr/bin/env python3
"""Pause hung 2.11BSD and dump PC/PS/nearby code."""

from __future__ import annotations

import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from benchmark_boot_times import TelnetConnection, SHELL_PROMPT_RE, SHELL_BANNER

HOST = "192.168.7.144"


def drain(conn: TelnetConnection, seconds: float) -> bytes:
    deadline = time.monotonic() + seconds
    data = bytearray()
    while time.monotonic() < deadline:
        chunk = conn.receive()
        if chunk:
            data.extend(chunk)
            sys.stdout.write(chunk.decode("latin-1", errors="replace"))
            sys.stdout.flush()
    return bytes(data)


def ensure_shell(conn: TelnetConnection) -> None:
    # Escape any leftover monitor/guest state.
    for seq in (b">\r", b"\r", b"\x1b>>"):
        conn.send(seq)
        data = drain(conn, 1.5)
        if SHELL_PROMPT_RE.search(data) or SHELL_BANNER in data:
            # Make sure we have a prompt
            conn.send(b"\r")
            drain(conn, 0.8)
            return
    conn.send(b"\x1b>>")
    deadline = time.monotonic() + 10
    buf = bytearray()
    while time.monotonic() < deadline:
        chunk = conn.receive()
        if chunk:
            buf.extend(chunk)
            sys.stdout.write(chunk.decode("latin-1", errors="replace"))
            sys.stdout.flush()
            if SHELL_PROMPT_RE.search(buf):
                return
    raise RuntimeError("could not enter management shell")


def shell(conn: TelnetConnection, cmd: str) -> None:
    conn.send(cmd.encode("ascii") + b"\r")
    drain(conn, 2.0)


def main() -> int:
    conn = TelnetConnection(HOST, 23, timeout=2.0)
    conn.connect()
    drain(conn, 0.5)
    ensure_shell(conn)

    print("\n--- lights (running) ---", flush=True)
    shell(conn, "lights")

    print("\n--- pause ---", flush=True)
    conn.send(b"monitor\r")
    drain(conn, 1.0)
    conn.send(b"P\r")
    drain(conn, 1.0)
    conn.send(b">\r")
    drain(conn, 0.5)

    print("\n--- lights (paused) ---", flush=True)
    shell(conn, "lights")

    print("\n--- dump ---", flush=True)
    conn.send(b"monitor\r")
    drain(conn, 0.8)
    for cmd in (
        b"D134560\r",
        b"D004360\r",
        b"U\r",
        b"S\r",
        b"S\r",
        b"S\r",
        b"S\r",
        b"S\r",
        b"S\r",
        b"S\r",
        b"S\r",
    ):
        conn.send(cmd)
        drain(conn, 1.0)

    conn.send(b">\r")
    drain(conn, 0.5)
    shell(conn, "lights")
    conn.send(b"exit\r")
    drain(conn, 0.5)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
