#!/usr/bin/env python3
"""After last good csv from 042562, catch FIRST trap entry (004332) with full dump."""

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
CSV = 0o003102
TRAP = 0o004332  # ASENTRY(trap) mov SSR etc — actually entry
# From dump, trap starts ~004332 with mov SSR0 path; call1 ~004000
CALL1 = 0o004014  # guess — will dump
IO_SP = 0o160000


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


def parse(blob: bytes) -> dict:
    m = re.search(
        rb"state: PC=([0-7]+) R0=([0-7]+) R1=([0-7]+) R2=([0-7]+) "
        rb"R3=([0-7]+) R4=([0-7]+) R5=([0-7]+) SP=([0-7]+) PS=([0-7]+) "
        rb"NEXT=[0-7]+:[0-7]+\s+([^\r\n]+)",
        blob,
    )
    if not m:
        return {}
    return {
        "pc": int(m.group(1), 8),
        "r0": int(m.group(2), 8),
        "r1": int(m.group(3), 8),
        "r2": int(m.group(4), 8),
        "r3": int(m.group(5), 8),
        "r4": int(m.group(6), 8),
        "r5": int(m.group(7), 8),
        "sp": int(m.group(8), 8),
        "ps": int(m.group(9), 8),
        "next": m.group(10).decode("latin-1", errors="replace").strip(),
    }


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
    mon(conn, "R3=000001", 0.3)

    # First dump trap/call1 locations
    print("\n>>> locate trap/call1", flush=True)
    mon(conn, "M004000", 1.2)
    mon(conn, "M004020", 1.2)
    mon(conn, "M004320", 1.2)
    mon(conn, "M004340", 1.2)
    mon(conn, "M052720", 1.2)
    mon(conn, "M042540", 1.2)
    mon(conn, "M042560", 1.2)

    # Vector 4 (odd address / NXM / red stack)
    print("\n>>> vectors 0-34", flush=True)
    mon(conn, "M000000", 1.2)

    mon(conn, f"B{CSV:06o}", 0.3)
    mon(conn, "C", 0.2)

    saw_42562 = 0
    for i in range(40):
        time.sleep(0.04)
        blob = mon(conn, "P", 0.85)
        st = parse(blob)
        if not st:
            if re.search(rb"PC=0005[0-2][0-7]", blob):
                mon(conn, "R3=000001", 0.3)
            mon(conn, "C", 0.15)
            continue
        pm = re.search(rb"previous: PC=([0-7]+)", blob)
        prev_pc = int(pm.group(1), 8) if pm else -1
        print(
            f"  csv[{i}] SP={st['sp']:06o} prev={prev_pc:06o} PS={st['ps']:06o}",
            flush=True,
        )
        if prev_pc == 0o042562 and st["sp"] < IO_SP:
            saw_42562 += 1
        # After a couple of 042562 calls, switch to trap break
        if saw_42562 >= 1 and st["sp"] < IO_SP:
            print("  >>> arm TRAP 004332, continue (no csv)", flush=True)
            mon(conn, f"B{TRAP:06o}", 0.3)
            mon(conn, "C", 0.2)
            break
        mon(conn, f"B{CSV:06o}", 0.2)
        mon(conn, "C", 0.15)
    else:
        print("failed to arm", flush=True)
        return 1

    for j in range(40):
        time.sleep(0.05)
        blob = mon(conn, "P", 1.0)
        st = parse(blob)
        if not st:
            mon(conn, "C", 0.15)
            continue
        print(
            f"  [{j}] PC={st['pc']:06o} SP={st['sp']:06o} PS={st['ps']:06o} "
            f"R0={st['r0']:06o} R5={st['r5']:06o} {st['next']}",
            flush=True,
        )
        # Always show previous line
        for line in blob.decode("latin-1", errors="replace").splitlines():
            if "previous:" in line or "last:" in line or "CPUERR" in line:
                print(f"      {line}", flush=True)

        if st["pc"] == TRAP or st["sp"] >= IO_SP or st["sp"] < 0o1000:
            print("\n*** FIRST interest — full dump", flush=True)
            mon(conn, "U", 2.0)
            # stacked PC/PS at SP (after hw push: SP->PC, SP+2->PS)
            mon(conn, f"M{st['sp']:06o}", 1.2)
            if st["sp"] >= 4 and st["sp"] < IO_SP:
                mon(conn, f"M{(st['sp']):06o}", 1.0)
            # low memory emergency stack area
            mon(conn, "M000000", 1.2)
            mon(conn, "D000000", 1.2)
            # nofault
            mon(conn, "M001540", 1.0)
            # saveps
            break
        mon(conn, f"B{TRAP:06o}", 0.2)
        mon(conn, "C", 0.15)

    mon(conn, "B clear", 0.3)
    mon(conn, ">", 0.2)
    shell_command(conn, "set break=0", 2.0, True)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
