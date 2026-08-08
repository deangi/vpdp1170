#!/usr/bin/env python3
"""Break on fault PC 016516 before it faults; dump SP/regs/code."""

from __future__ import annotations

import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from benchmark_boot_times import (  # noqa: E402
    BootProfile,
    TelnetConnection,
    install_config,
    shell_command,
    BenchmarkError,
    SHELL_PROMPT_RE,
    SHELL_BANNER,
)

HOST = "192.168.7.144"
FAULT_PC = "016516"


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
    for seq in (b">\r", b"\r", b"\x1b>>"):
        conn.send(seq)
        data = drain(conn, 0.7)
        if SHELL_PROMPT_RE.search(data) or SHELL_BANNER in data:
            conn.send(b"\r")
            drain(conn, 0.3)
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
    raise RuntimeError("no shell")


def mon(conn: TelnetConnection, cmd: str, wait: float = 1.0) -> bytes:
    conn.send(cmd.encode("ascii") + b"\r")
    return drain(conn, wait)


def main() -> int:
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
    shell_command(conn, "set break=0", 2.0, True)
    conn.send(b"monitor\r")
    drain(conn, 0.4)
    mon(conn, "B clear", 0.3)
    mon(conn, "C", 0.2)
    mon(conn, ">", 0.2)
    conn.send(b"reset\rexit\r")
    drain(conn, 2.0)

    print("\n--- boot to banner, then arm B016516 ---", flush=True)
    time.sleep(0.4)
    conn.send(b"\r")
    prompts = 1
    transcript = bytearray()
    deadline = time.monotonic() + 180.0
    while time.monotonic() < deadline:
        try:
            chunk = conn.receive()
        except BenchmarkError:
            chunk = b""
        if not chunk:
            continue
        transcript.extend(chunk)
        sys.stdout.write(chunk.decode("latin-1", errors="replace"))
        sys.stdout.flush()
        stripped = bytes(transcript).rstrip(b"\x00\r\n\t ")
        if prompts < 3 and (
            stripped.endswith(b":") or stripped.endswith(b"\x7f:")
        ):
            time.sleep(0.35)
            conn.send(b"rl(0,0,0)unix\r")
            prompts += 1
        if b"2.11 BSD" in transcript or b"RETRONFP" in transcript:
            break
    else:
        return 1

    # Pause ASAP, arm fault PC break, continue (don't truncate clear).
    ensure_shell(conn)
    conn.send(b"monitor\r")
    drain(conn, 0.4)
    mon(conn, "P", 1.0)
    mon(conn, f"B{FAULT_PC}", 0.5)
    mon(conn, "B", 0.4)
    print("\narmed; continuing full clear+...", flush=True)
    mon(conn, "C", 0.3)
    mon(conn, ">", 0.2)
    conn.send(b"exit\r")
    drain(conn, 0.3)

    deadline = time.monotonic() + 60.0
    next_poll = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        try:
            chunk = conn.receive()
        except BenchmarkError:
            chunk = b""
        if chunk and re.search(rb"BREAK pc=", chunk, re.I):
            print("\n*** BREAK ***", flush=True)
            break
        if time.monotonic() < next_poll:
            continue
        next_poll = time.monotonic() + 1.5
        ensure_shell(conn)
        lights = shell_command(conn, "lights", 1.5, True)
        if re.search(rb"address=0*016516\b", lights):
            break
        # Also catch if we landed in trap instead
        if re.search(rb"address=0*004332\b", lights):
            print("hit trap instead of 016516", flush=True)
            break
        conn.send(b"exit\r")
        drain(conn, 0.2)

    print("\n======== DUMP AT FAULT PC ========", flush=True)
    ensure_shell(conn)
    shell_command(conn, "lights", 1.5, True)
    conn.send(b"monitor\r")
    drain(conn, 0.5)
    print(mon(conn, "P", 1.5).decode("latin-1", errors="replace")[-800:], flush=True)
    print("\n>>> U", flush=True)
    mon(conn, "U", 2.0)
    print("\n>>> code at PC", flush=True)
    mon(conn, f"M{FAULT_PC}", 1.2)
    mon(conn, "M016470", 1.2)
    mon(conn, "M016500", 1.2)
    # Stack
    m = re.search(rb"SP=([0-7]+)", drain(conn, 0.1))
    # get SP from fresh P
    blob = mon(conn, "P", 1.0)
    m = re.search(rb"SP=([0-7]+)", blob)
    if m:
        sp = int(m.group(1), 8)
        print(f"\n>>> stack SP={sp:06o}", flush=True)
        mon(conn, f"D{sp:06o}", 1.2)
        mon(conn, f"D{(sp - 0o20) & 0o177777:06o}", 1.2)
    # Step one — does it fault?
    print("\n>>> step (may fault)", flush=True)
    for i in range(3):
        s = mon(conn, "S", 1.0)
        print(f"  [{i}] ...", flush=True)
        # show last state line
        lines = [
            ln
            for ln in s.decode("latin-1", errors="replace").splitlines()
            if "state:" in ln or "BREAK" in ln or "pause" in ln
        ]
        for ln in lines[-2:]:
            print(f"  {ln}", flush=True)

    mon(conn, "B clear", 0.3)
    mon(conn, ">", 0.2)
    shell_command(conn, "set break=0", 2.0, True)
    conn.close()
    print("\ndone", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
