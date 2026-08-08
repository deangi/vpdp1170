#!/usr/bin/env python3
"""After clear, zero STACKLIM (disable red/yellow) and see if boot proceeds."""

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
    raise RuntimeError("no shell")


def mon(conn: TelnetConnection, cmd: str, wait: float = 0.9) -> bytes:
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
    drain(conn, 0.3)
    mon(conn, "B clear", 0.3)
    mon(conn, "C", 0.2)
    mon(conn, ">", 0.2)
    conn.send(b"reset\rexit\r")
    drain(conn, 2.0)

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

    ensure_shell(conn)
    conn.send(b"monitor\r")
    drain(conn, 0.4)
    mon(conn, "P", 1.0)
    # Finish clear(s) fast
    mon(conn, "R3=000001", 0.3)
    mon(conn, "B000506", 0.3)

    # Zero STACKLIM via MMU I/O mapping: VA 177774 with KDSA7=177600
    # Writing STACKLIM: use monitor — is there a way?
    # Physical I/O isn't in RAM. Need to write through CPU register interface.
    # Check if W to high phys works... STACKLIM is CPU register at 177774,
    # handled in bus write_IO — need a guest store or emulator hook.
    #
    # Deposit via running a tiny store: set PC to a MOV #0, @#177774; HALT
    # Plant in low phys RAM that's mapped in I+D for a moment — messy.
    #
    # Simpler: use R deposits if there's stack limit set... there isn't.
    #
    # Write through guest: patch and execute
    # At phys 2000 (unused low mem during kernel run — vectors in use but
    # 2000 might be ok for a tiny stub). Kernel D page0 maps phys 0.
    # Plant at VA 2000 in D/I: with sep I/D, need I-space write.
    # KISA0=001302 → VA 2000 → phys 00130200+2000 = 00132200
    print("\n>>> plant STACKLIM=0 stub at VA 002000 / phys 132200", flush=True)
    # MOV #0, @#177774  = 012737 000000 177774
    # HALT              = 000000
    mon(conn, "W132200=012737", 0.4)
    mon(conn, "W132202=000000", 0.4)
    mon(conn, "W132204=177774", 0.4)
    mon(conn, "W132206=000000", 0.4)  # HALT
    mon(conn, "D132200", 0.8)

    # Save PC/PS/SP and jump to stub
    blob = mon(conn, "P", 0.8)
    print("before stub:", blob.decode("latin-1", errors="replace")[-300:], flush=True)
    mon(conn, "PC=002000", 0.4)
    mon(conn, "PS=000340", 0.4)  # kernel, pri7
    mon(conn, "C", 0.3)
    time.sleep(0.2)
    blob = mon(conn, "P", 1.0)
    print("after stub:", blob.decode("latin-1", errors="replace")[-400:], flush=True)
    mon(conn, "M177774", 1.0)

    # Restore: continue clear with R3=1 — need to get back to clear
    # Re-pause was mid-clear; set R3=1, PC back into clear loop if needed
    mon(conn, "R3=000001", 0.3)
    # If HALTed at 002006, resume clear at 000506
    mon(conn, "PC=000506", 0.4)
    mon(conn, "SP=147500", 0.4)
    mon(conn, "PS=030344", 0.4)
    mon(conn, "B clear", 0.3)
    print("\n>>> resume boot with STACKLIM=0; watch console 90s", flush=True)
    mon(conn, "C", 0.2)
    mon(conn, ">", 0.2)
    # exit to console
    conn.send(b"exit\r")
    drain(conn, 1.0)

    deadline = time.monotonic() + 90.0
    buf = bytearray()
    while time.monotonic() < deadline:
        try:
            chunk = conn.receive()
        except BenchmarkError:
            chunk = b""
        if chunk:
            buf.extend(chunk)
            sys.stdout.write(chunk.decode("latin-1", errors="replace"))
            sys.stdout.flush()
            low = bytes(buf).lower()
            if b"phys mem" in low or b"login:" in low or b"#" in buf[-20:]:
                print("\n*** BOOT PROGRESS DETECTED ***", flush=True)
                break
            if b"panic" in low:
                print("\n*** PANIC ***", flush=True)
                break
    else:
        print("\n*** no phys mem / login in 90s — probing CPU ***", flush=True)
        ensure_shell(conn)
        conn.send(b"monitor\r")
        drain(conn, 0.4)
        mon(conn, "P", 1.2)
        mon(conn, "U", 2.0)
        mon(conn, "M177774", 1.0)

    ensure_shell(conn)
    shell_command(conn, "set break=0", 2.0, True)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
