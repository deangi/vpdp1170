#!/usr/bin/env python3
"""Answer ':' with CR, boot 2.11BSD, dump state after phys-mem hang."""

from __future__ import annotations

import sys
import time
from datetime import datetime
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
OUT = ROOT / "boot-benchmark-results-211bsd-postphys"


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


def main() -> int:
    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    log_path = OUT / f"{stamp}-continue-boot.log"

    conn = TelnetConnection(HOST, 23, timeout=2.0)
    for attempt in range(20):
        try:
            conn.connect()
            break
        except Exception as exc:
            print(f"telnet wait {attempt}: {exc}", flush=True)
            time.sleep(2)
            conn = TelnetConnection(HOST, 23, timeout=2.0)
    else:
        raise RuntimeError("telnet down")

    # Prefer PDP console. If we land in shell, exit first.
    first = drain(conn, 1.0)
    if SHELL_PROMPT_RE.search(first) or SHELL_BANNER in first or b"vpdp:/>" in first:
        print("\n=== leaving shell for PDP console ===\n", flush=True)
        sh(conn, "set pcping=0", 1.0)
        sh(conn, "exit", 0.8)

    print("\n=== send CR to boot prompt ===\n", flush=True)
    conn.send(b"\r")
    time.sleep(0.5)
    # A couple more in case first was eaten
    for _ in range(3):
        conn.send(b"\r")
        time.sleep(0.4)

    buf = bytearray(first)
    start = time.monotonic()
    last_out = start
    phys_at = None
    armed = False
    deadline = start + 240.0
    markers = (b"2.11 BSD", b"phys mem", b"configure", b"attached", b"login:", b"# ")
    seen = set()

    while time.monotonic() < deadline:
        now = time.monotonic()
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
            for m in markers:
                if m not in seen and m in buf:
                    seen.add(m)
                    print(f"\n*** MARKER {m.decode(errors='replace')} ***\n", flush=True)
            if (b"2.11 BSD" in buf or b"phys mem" in buf) and not armed:
                print("\n*** arming traces ***\n", flush=True)
                ensure_shell(conn)
                sh(conn, "set pcping=0", 1.0)
                sh(conn, "set clock_trace=100", 1.0)
                sh(conn, "set dl_trace=200", 1.0)
                sh(conn, "exit", 0.5)
                armed = True
                last_out = time.monotonic()
            if b"phys mem" in buf and phys_at is None:
                phys_at = now
            if b"login: " in buf or b"configure system" in buf:
                break
            continue

        if phys_at is not None and now - last_out > 35.0:
            print("\n*** quiet after phys mem ***\n", flush=True)
            break
        if buf and phys_at is None and now - last_out > 90.0:
            print("\n*** quiet before phys mem ***\n", flush=True)
            break

    log_path.write_bytes(buf)
    print(
        f"\n=== capture {len(buf)} bytes markers={sorted(x.decode(errors='replace') for x in seen)} ===\n",
        flush=True,
    )

    ensure_shell(conn)
    for cmd in ("rl regs", "tty", "lights"):
        print(f"\n>>> {cmd}", flush=True)
        sh(conn, cmd, 2.5)

    conn.send(b"monitor\r")
    drain(conn, 0.4)
    for cmd, w in (("P", 1.5), ("U", 2.5), (">", 0.4)):
        print(f"\n>>> monitor {cmd}", flush=True)
        conn.send(cmd.encode() + b"\r")
        drain(conn, w)

    sh(conn, "set clock_trace=0", 1.0)
    sh(conn, "set dl_trace=0", 1.0)
    sh(conn, "exit", 0.5)
    conn.close()
    print(f"\nlog: {log_path}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
