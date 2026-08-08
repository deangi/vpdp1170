#!/usr/bin/env python3
"""Patch vector trap entry to HALT so first trap freezes with intact regs/SP."""

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
TRAP = 0o004332


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
        rb"R3=([0-7]+) R4=([0-7]+) R5=([0-7]+) SP=([0-7]+) PS=([0-7]+)",
        blob,
    )
    if not m:
        return {}
    return {k: int(m.group(i), 8) for i, k in enumerate(
        ["pc", "r0", "r1", "r2", "r3", "r4", "r5", "sp", "ps"], 1
    )}


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

    # Physical address of trap entry: KISA0=001302 → VA 004332 maps to
    # phys = 00130200 + (004332 - 0) but page0 I is 8KB from 00130200.
    # VA 004332 is in page 0 (APF0), offset 04332.
    # I-phys page0 = 00130200, so phys = 00130200 + 04332 = 00134532
    #
    # Safer: use W through monitor if it writes physical, or patch via M if
    # M write exists. Check: W is physical. Compute phys from U dump.
    # From earlier U: I-PAR0=001302 → phys word addr = (001302 << 6) + 04332
    # PAR is in 64-byte clicks: phys_byte = PAR * 64 + offset
    # In PDP-11, addresses in dumps are often word-oriented in 18/22-bit.
    # kek uses 22-bit byte addresses? D dump showed D000000 = vectors in bytes?
    #
    # Actually D00100 dumps words at physical. Vector at phys 0.
    # For I-space VA 004332 with KISA0=001302 (clicks):
    # physical = 001302 * 0100 + 004332 = 00130200 + 004332 = 00134532

    print("\n>>> dump trap insn before patch", flush=True)
    mon(conn, "M004332", 1.0)
    mon(conn, "D134532", 1.0)  # try computed phys

    # Patch via depositing HALT at virtual using... W is physical only.
    # Use W at computed phys. Also try patching vector 4 to point to a HALT
    # we plant at physical 1000.
    print("\n>>> plant HALT at phys 001000 and repoint vector 4", flush=True)
    mon(conn, "W001000=000000", 0.5)  # HALT
    # Vector 4 at phys 4: PC, phys 6: PS. Currently 004332, 000340
    # Redirect vector 4 to 001000 (our HALT), keep PS 000340
    mon(conn, "W000004=001000", 0.5)
    mon(conn, "W000006=000340", 0.5)
    mon(conn, "D000000", 1.0)
    mon(conn, "D001000", 0.8)

    # Also need 2nd clear patch
    mon(conn, "B000506", 0.3)
    print("\n>>> continue until HALT/stop", flush=True)
    mon(conn, "C", 0.3)

    for i in range(60):
        time.sleep(0.08)
        blob = mon(conn, "P", 1.0)
        st = parse(blob)
        if not st:
            # clear patch?
            if re.search(rb"PC=0005", blob):
                mon(conn, "R3=000001", 0.3)
            mon(conn, "C", 0.15)
            continue
        print(
            f"  [{i}] PC={st['pc']:06o} SP={st['sp']:06o} PS={st['ps']:06o} "
            f"R0={st['r0']:06o} R1={st['r1']:06o} R2={st['r2']:06o} "
            f"R5={st['r5']:06o}",
            flush=True,
        )
        for line in blob.decode("latin-1", errors="replace").splitlines():
            if "previous:" in line or "last:" in line or "HALT" in line or "stopped" in line:
                print(f"      {line}", flush=True)

        if st["pc"] == 0o1000 or st["sp"] < 0o2000 or st["sp"] >= 0o160000:
            print("\n*** caught first vector-4 path", flush=True)
            mon(conn, "U", 2.0)
            mon(conn, "D000000", 1.2)
            # stacked frame at SP
            mon(conn, f"D{st['sp']:06o}", 1.2)
            if st["sp"] >= 4:
                # PC at SP, PS at SP+2 after trap stack
                mon(conn, f"D{(st['sp']):06o}", 1.0)
            mon(conn, "M177764", 1.0)  # CPUERR
            mon(conn, "M177776", 1.0)
            break

        if 0o500 <= st["pc"] <= 0o520 and st["r3"] > 2:
            mon(conn, "R3=000001", 0.3)
        mon(conn, "C", 0.15)
    else:
        print("no catch", flush=True)

    mon(conn, "B clear", 0.3)
    mon(conn, ">", 0.2)
    shell_command(conn, "set break=0", 2.0, True)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
