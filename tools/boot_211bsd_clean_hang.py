#!/usr/bin/env python3
"""Clean 211bsd boot: no mid-boot shell; dump hang after user mem."""

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
KWP = sys.argv[1] if len(sys.argv) > 1 else "false"  # false|true


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
    telnet_log = OUT / f"{stamp}-clean-kwp{KWP}-telnet.log"

    conn = TelnetConnection(HOST, 23, timeout=2.0)
    for attempt in range(15):
        try:
            conn.connect()
            break
        except Exception as exc:
            print("telnet", attempt, exc, flush=True)
            time.sleep(2)
            conn = TelnetConnection(HOST, 23, timeout=2.0)
    else:
        raise RuntimeError("telnet down")

    ensure_shell(conn)
    profile = BootProfile(
        name="211bsd",
        config_path="/pdpconfig-211bsd.ini",
        completion=b"login: ",
        quiet_seconds=2.0,
    )
    install_config(conn, profile, 12.0, True)
    shell_command(conn, "set pcping=0", 1.0, True)
    # kwp_enabled is config-file only; patch via runtime is not supported.
    shell_command(conn, "set", 2.0, True)

    conn.send(b"reset\r")
    time.sleep(0.25)
    conn.send(b"exit\r")
    drain(conn, 1.0)

    buf = bytearray()
    start = time.monotonic()
    last_out = start
    last_cr = 0.0
    cr_n = 0
    user_mem_at = None
    deadline = start + 200.0

    while time.monotonic() < deadline:
        now = time.monotonic()
        if cr_n < 20 and now - start < 45 and b"2.11 BSD" not in buf and now - last_cr >= 2.0:
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
            if b"user mem" in buf and user_mem_at is None:
                user_mem_at = now
                print("\n*** saw user mem — wait for progress ***\n", flush=True)
            if b"login: " in buf or b"configure system" in buf:
                print("\n*** BOOT PROGRESS ***\n", flush=True)
                break
            continue
        if user_mem_at and now - last_out > 35:
            print("\n*** quiet after user mem — dump ***\n", flush=True)
            break

    telnet_log.write_bytes(buf)
    ensure_shell(conn)
    print(f"\n=== hang dump (kwp={KWP}) ===\n", flush=True)
    for cmd in ("rl regs", "lights", "tty"):
        print(f">>> {cmd}", flush=True)
        sh(conn, cmd, 2.0)
    # clock command may not exist until flash
    print(">>> clock", flush=True)
    sh(conn, "clock", 1.5)

    conn.send(b"monitor\r")
    drain(conn, 0.4)
    for cmd, w in (
        ("P", 1.5),
        ("MI021450", 1.2),
        ("MI003100", 1.2),
        ("MI025500", 1.0),
        ("D077122", 1.0),
        ("U", 2.5),
        ("C", 0.3),
        (">", 0.3),
    ):
        print(f">>> mon {cmd}", flush=True)
        conn.send(cmd.encode() + b"\r")
        drain(conn, w)

    sh(conn, "exit", 0.5)
    conn.close()
    print(f"\ntelnet={telnet_log}", flush=True)
    if b"configure system" in buf or b"login: " in buf:
        return 0
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
