#!/usr/bin/env python3
"""Boot 2.11BSD: attach console ASAP, inject CR, dump rl regs on hang."""

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
CAPTURE_SECS = 300.0


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
        data = drain(conn, 0.8)
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
    shell_command(conn, "set pcping=0", 1.5, True)
    shell_command(conn, "set break=0", 1.5, True)
    # Host-side CR as well as boot_script, in case ':' was missed.
    shell_command(conn, 'set boot_input=\\r', 1.5, True)
    shell_command(conn, 'set boot_script=: => \\r', 1.5, True)
    sh(conn, "set", 2.0)

    print("\n=== reset + immediate exit ===\n", flush=True)
    conn.send(b"reset\r")
    # Minimal wait so the command is accepted, then release console.
    time.sleep(0.15)
    conn.send(b"exit\r")
    drain(conn, 1.0)

    print(f"\n=== capturing {CAPTURE_SECS:.0f}s (CR bursts early) ===\n", flush=True)
    buf = bytearray()
    start = time.monotonic()
    deadline = start + CAPTURE_SECS
    last_output = start
    last_cr = 0.0
    cr_bursts = 0
    quiet_after_phys = None
    markers = (
        b"2.11 BSD",
        b"phys mem",
        b"avail mem",
        b"user mem",
        b"configure",
        b"attached",
        b"login:",
        b"# ",
        b"panic",
    )
    seen = set()

    while time.monotonic() < deadline:
        now = time.monotonic()
        # For the first 60s, poke CR every 2s until we see banner/phys mem.
        if (
            cr_bursts < 30
            and now - start < 60.0
            and b"phys mem" not in buf
            and b"2.11 BSD" not in buf
            and now - last_cr >= 2.0
        ):
            conn.send(b"\r")
            last_cr = now
            cr_bursts += 1
            print(f"[host CR #{cr_bursts}]\n", flush=True)

        try:
            chunk = conn.receive()
        except BenchmarkError:
            time.sleep(0.05)
            continue

        if chunk:
            buf.extend(chunk)
            last_output = now
            sys.stdout.write(chunk.decode("latin-1", errors="replace"))
            sys.stdout.flush()
            for m in markers:
                if m not in seen and m in buf:
                    seen.add(m)
                    print(f"\n*** MARKER {m.decode(errors='replace')} ***\n", flush=True)
            if b"phys mem" in buf and quiet_after_phys is None:
                quiet_after_phys = now
            if b"login: " in buf or b"\n# " in buf:
                break
            continue

        if quiet_after_phys is not None and now - last_output > 40.0:
            print("\n*** quiet 40s after phys mem — dump ***\n", flush=True)
            break
        if buf and quiet_after_phys is None and now - last_output > 75.0:
            print("\n*** quiet 75s — dump ***\n", flush=True)
            break
        if not buf and now - start > 90.0:
            print("\n*** no console bytes in 90s — dump ***\n", flush=True)
            break

    log_path.write_bytes(buf)
    print(
        f"\n=== capture: {len(buf)} bytes markers={sorted(x.decode(errors='replace') for x in seen)} ===\n",
        flush=True,
    )

    ensure_shell(conn)
    print("\n=== dump ===\n", flush=True)
    for cmd in ("tty", "lights", "rl regs", "rp status"):
        print(f"\n>>> {cmd}", flush=True)
        sh(conn, cmd, 2.5)

    conn.send(b"monitor\r")
    drain(conn, 0.4)
    for cmd, w in (("P", 1.5), ("U", 2.5), ("D077122", 1.2), (">", 0.4)):
        print(f"\n>>> monitor {cmd}", flush=True)
        conn.send(cmd.encode("ascii") + b"\r")
        drain(conn, w)

    sh(conn, "exit", 0.8)
    conn.close()
    print(f"\nlog: {log_path}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
