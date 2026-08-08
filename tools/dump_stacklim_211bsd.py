#!/usr/bin/env python3
"""At banner/clear pause: dump STACKLIM, intstk guess, then trace red-stack."""

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
ADDR_STACKLIM = 0o177774


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
    mon(conn, "P", 1.2)
    print("\n>>> STACKLIM (177774) and neighbors", flush=True)
    mon(conn, "M177770", 1.2)
    mon(conn, "D177770", 1.0)  # physical may differ
    print("\n>>> U / CPUERR", flush=True)
    mon(conn, "U", 2.0)
    print("\n>>> nofault and nearby vars (001520)", flush=True)
    mon(conn, "M001520", 1.0)
    # eintstk / intstk often near nofault in bss — dump low D
    print("\n>>> low D-space (bss intstk?)", flush=True)
    mon(conn, "M000000", 1.0)
    mon(conn, "M001000", 1.0)
    mon(conn, "M001400", 1.0)
    mon(conn, "M001500", 1.0)

    # Finish clear fast, break on SP-sensitive trap vector path;
    # also break when PC in cret/csv after clear.
    mon(conn, "R3=000001", 0.4)
    mon(conn, "B000506", 0.3)
    # Watch mem_parity / fioword: break on trap AND on 004332
    mon(conn, "B004332", 0.3)
    print("\n>>> continue; stop on clear-loop or trap", flush=True)
    mon(conn, "C", 0.3)

    for i in range(40):
        time.sleep(0.08)
        blob = mon(conn, "P", 0.9)
        text = blob.decode("latin-1", errors="replace")
        m = re.search(
            rb"PC=([0-7]+) .* SP=([0-7]+) PS=([0-7]+)",
            blob,
        )
        if not m:
            mon(conn, "C", 0.2)
            continue
        pc, sp, ps = (int(m.group(j), 8) for j in (1, 2, 3))
        r3m = re.search(rb"R3=([0-7]+)", blob)
        r3 = int(r3m.group(1), 8) if r3m else -1
        print(f"  [{i}] PC={pc:06o} SP={sp:06o} PS={ps:06o} R3={r3:06o}", flush=True)

        # Dump STACKLIM each time SP looks weird or on trap
        if sp < 0o1000 or sp >= 0o160000 or pc == 0o4332:
            print("  *** interesting SP/PC — STACKLIM dump ***", flush=True)
            mon(conn, "M177774", 1.0)
            mon(conn, "U", 2.0)
            mon(conn, f"D{sp:06o}", 1.0)
            if pc == 0o4332:
                # stacked pc/ps
                mon(conn, f"M{sp:06o}", 1.0)
            break

        if 0o500 <= pc <= 0o520 and r3 > 2:
            mon(conn, "R3=000001", 0.3)
        mon(conn, "C", 0.2)
    else:
        mon(conn, "M177774", 1.0)

    mon(conn, "B clear", 0.3)
    mon(conn, ">", 0.2)
    shell_command(conn, "set break=0", 2.0, True)
    conn.close()
    print("\ndone", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
