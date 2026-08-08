#!/usr/bin/env python3
"""Catch the FIRST kernel trap after 2.11BSD banner (before storm).

Sequence:
  1. Boot with no break
  2. On banner, immediately pause
  3. Arm B004332 (kernel trap entry)
  4. Continue; first break is the original fault
  5. Dump stacked PC/PS, MMR*, CPUERR, nofault, UISA
"""

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
TRAP_PC = "004332"


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
        data = drain(conn, 0.9)
        if SHELL_PROMPT_RE.search(data) or SHELL_BANNER in data:
            conn.send(b"\r")
            drain(conn, 0.4)
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

    print("\n--- install, clear break, reset ---", flush=True)
    install_config(conn, profile, 15.0, True)
    shell_command(conn, "set break=0", 3.0, True)
    conn.send(b"monitor\r")
    drain(conn, 0.5)
    mon(conn, "B clear", 0.5)
    mon(conn, "C", 0.3)  # in case left paused
    mon(conn, ">", 0.3)
    conn.send(b"reset\rexit\r")
    drain(conn, 2.0)

    print("\n--- boot until banner ---", flush=True)
    time.sleep(0.5)
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
            time.sleep(0.4)
            conn.send(b"rl(0,0,0)unix\r")
            prompts += 1
            print(f"\n  answered : #{prompts}", flush=True)
        if b"2.11 BSD" in transcript or b"RETRONFP" in transcript:
            print("\n*** BANNER - pause NOW ***", flush=True)
            break
    else:
        print("timeout waiting for banner", flush=True)
        return 1

    # Immediate pause before probe/clear advances far.
    ensure_shell(conn)
    conn.send(b"monitor\r")
    drain(conn, 0.5)
    pause = mon(conn, "P", 1.5)
    print("\n>>> paused after banner", flush=True)
    mon(conn, "U", 2.0)
    mon(conn, ">", 0.3)
    ensure_shell(conn)
    shell_command(conn, "lights", 2.0, True)

    print("\n--- arm trap break, continue ---", flush=True)
    shell_command(conn, f"set break={TRAP_PC}", 3.0, True)
    conn.send(b"monitor\r")
    drain(conn, 0.5)
    mon(conn, f"B{TRAP_PC}", 0.8)
    mon(conn, "B", 0.5)
    mon(conn, "C", 0.4)
    mon(conn, ">", 0.3)
    conn.send(b"exit\r")
    drain(conn, 0.3)

    print("\n--- wait for first trap break ---", flush=True)
    deadline = time.monotonic() + 30.0
    next_poll = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        try:
            chunk = conn.receive()
        except BenchmarkError:
            chunk = b""
        if chunk:
            sys.stdout.write(chunk.decode("latin-1", errors="replace"))
            sys.stdout.flush()
            if re.search(rb"BREAK pc=", chunk, re.I):
                break
        if time.monotonic() < next_poll:
            continue
        next_poll = time.monotonic() + 1.5
        ensure_shell(conn)
        lights = shell_command(conn, "lights", 1.8, True)
        if re.search(rb"address=0*004332\b", lights):
            conn.send(b"monitor\r")
            drain(conn, 0.5)
            if b"currently paused" in mon(conn, "?", 0.7):
                break
            mon(conn, ">", 0.3)
            conn.send(b"exit\r")
            drain(conn, 0.2)
            continue
        conn.send(b"exit\r")
        drain(conn, 0.2)

    print("\n======== FIRST TRAP DUMP ========", flush=True)
    ensure_shell(conn)
    shell_command(conn, "lights", 2.0, True)
    conn.send(b"monitor\r")
    drain(conn, 0.6)
    st = mon(conn, "P", 1.5)
    print("\n>>> U", flush=True)
    u = mon(conn, "U", 2.5)

    # Stacked PC/PS at SP (trap pushes PS then PC; SP -> PC)
    msp = re.search(rb"SP=([0-7]+)", st)
    sp = int(msp.group(1), 8) if msp else 0o177750
    print(f"\n>>> stack at SP={sp:06o}", flush=True)
    stack = mon(conn, f"D{sp:06o}", 1.5)
    # Also SP-4 in case we're mid-handler
    mon(conn, f"D{(sp - 0o10) & 0o177777:06o}", 1.2)

    print("\n>>> step: tst nofault", flush=True)
    for i in range(6):
        s = mon(conn, "S", 0.9)
        m = re.search(
            rb"state: PC=([0-7]+) .* SP=([0-7]+) PS=([0-7]+) "
            rb"NEXT=[0-7]+:[0-7]+\s+([^\r\n]+)",
            s,
        )
        if m:
            print(
                f"  [{i}] PC={m.group(1).decode()} SP={m.group(2).decode()} "
                f"PS={m.group(3).decode()} {m.group(4).decode().strip()}",
                flush=True,
            )

    print("\n>>> nofault + clear region", flush=True)
    mon(conn, "D001520", 1.2)
    mon(conn, "D000500", 1.2)
    # Fault PC from stack words — dump that code
    words = re.findall(rb"\b([0-7]{6})\b", stack)
    if len(words) >= 2:
        # D dump format: addr: w0 w1 w2...
        fault_pc = words[1] if len(words) > 1 else words[0]
        # First data word after address is usually PC
        m2 = re.search(rb"^[0-7]{6}:\s+([0-7]+)\s+([0-7]+)", stack, re.M)
        if m2:
            fault_pc = m2.group(1).decode()
            fault_ps = m2.group(2).decode()
            print(f"\n*** stacked fault PC={fault_pc} PS={fault_ps} ***", flush=True)
            mon(conn, f"D{fault_pc}", 1.5)
            # Also physical/logical around fault
            mon(conn, f"M{fault_pc}", 1.5)

    mm = re.search(
        rb"MMU: MMR0=([0-7]+) MMR1=([0-7]+) MMR2=([0-7]+) MMR3=([0-7]+)",
        u,
    )
    if mm:
        print(
            f"\nMMR0={mm.group(1).decode()} MMR1={mm.group(2).decode()} "
            f"MMR2={mm.group(3).decode()} MMR3={mm.group(4).decode()}",
            flush=True,
        )
    ce = re.search(rb"CPUERR=([0-7]+)", u)
    if ce:
        print(f"CPUERR={ce.group(1).decode()}", flush=True)

    mon(conn, "B clear", 0.5)
    mon(conn, ">", 0.3)
    shell_command(conn, "set break=0", 2.0, True)
    shell_command(conn, "lights", 2.0, True)
    conn.close()
    print("\ndone", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
