#!/usr/bin/env python3
"""Cold-boot 2.11BSD; capture console past phys mem (no pcping)."""

from __future__ import annotations

import sys
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from benchmark_boot_times import (  # noqa: E402
    BootProfile,
    TelnetConnection,
    BenchmarkError,
    SHELL_PROMPT_RE,
    SHELL_BANNER,
    install_config,
    shell_command,
)

HOST = "192.168.7.144"
OUT = ROOT / "boot-benchmark-results-211bsd-postphys"
CAPTURE_SECS = 180.0


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


def main() -> int:
    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    log_path = OUT / f"{stamp}-boot.log"

    profile = BootProfile(
        name="211bsd",
        config_path="/pdpconfig-211bsd.ini",
        completion=b"login: ",
        quiet_seconds=2.0,
    )

    conn = TelnetConnection(HOST, 23, timeout=2.0)
    conn.connect()
    ensure_shell(conn)
    install_config(conn, profile, 12.0, True)
    shell_command(conn, "set pcping=0", 2.0, True)
    shell_command(conn, "set break=0", 2.0, True)
    shell_command(conn, "set dl_trace=200", 2.0, True)
    shell_command(conn, "set clock_trace=50", 2.0, True)

    print("\n=== reset ===\n", flush=True)
    conn.send(b"reset\r")
    drain(conn, 2.0)
    conn.send(b"exit\r")
    drain(conn, 1.0)

    print(f"\n=== capturing {CAPTURE_SECS:.0f}s to {log_path.name} ===\n", flush=True)
    buf = bytearray()
    deadline = time.monotonic() + CAPTURE_SECS
    markers = (b"phys mem", b"configure", b"attached", b"login:", b"# ", b"panic")
    seen = set()
    while time.monotonic() < deadline:
        try:
            chunk = conn.receive()
        except BenchmarkError:
            time.sleep(0.05)
            continue
        if not chunk:
            continue
        buf.extend(chunk)
        sys.stdout.write(chunk.decode("latin-1", errors="replace"))
        sys.stdout.flush()
        for m in markers:
            if m not in seen and m in buf:
                seen.add(m)
                print(f"\n*** MARKER {m.decode()} ***\n", flush=True)
        if b"login: " in buf or b"\n# " in buf:
            break

    log_path.write_bytes(buf)
    print(f"\n=== done, {len(buf)} bytes, markers={sorted(x.decode() for x in seen)} ===", flush=True)

    # Snapshot RL-ish state via shell
    ensure_shell(conn)
    shell_command(conn, "set dl_trace=0", 1.0, True)
    shell_command(conn, "set clock_trace=0", 1.0, True)
    shell_command(conn, "tty", 2.0, True)
    shell_command(conn, "lights", 1.0, True)
    shell_command(conn, "rp status", 1.0, True)
    conn.send(b"monitor\r")
    drain(conn, 0.5)
    conn.send(b"P\r")
    drain(conn, 1.5)
    conn.send(b"U\r")
    drain(conn, 2.5)
    conn.send(b"D077122\r")
    drain(conn, 1.0)
    conn.send(b">\r")
    drain(conn, 0.3)
    conn.send(b"exit\r")
    drain(conn, 1.0)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
