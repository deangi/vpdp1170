#!/usr/bin/env python3
"""Finish clear; break every trap entry; report first nofault==0 with SP state."""

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
# nofault follows intstk; STACKLIM was intstk-256=001456 → intstk=002056
# INTSTK=500. → nofault at 002056+500. = 003042
NOFAULT = 0o003042
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


def read_word(conn: TelnetConnection, addr: int) -> int | None:
    blob = mon(conn, f"M{addr:06o}", 0.7)
    # line like "003042: 000000 012345 ..."
    m = re.search(rb"%06o:\s*([0-7]+)" % addr, blob)
    if not m:
        m = re.search(rb":\s*([0-7]+)", blob)
    return int(m.group(1), 8) if m else None


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

    # Verify nofault address: scan for a zero word near end of intstk
    print("\n>>> probe nofault candidates", flush=True)
    for a in (0o003042, 0o003040, 0o002056, 0o001656, 0o001520):
        w = read_word(conn, a)
        print(f"  M{a:06o} = {w if w is None else f'{w:06o}'}", flush=True)

    # Also find nofault by symbol pattern used in catchfault: known from
    # earlier dumps — search 001500-003100 for suspicious slots
    mon(conn, "M003000", 1.0)
    mon(conn, "M003040", 1.0)

    # Patch clear loop break; arm trap immediately
    mon(conn, "B000506", 0.3)  # re-patch 2nd clear
    mon(conn, f"B{TRAP:06o}", 0.3)
    print("\n>>> trap watch from end of clear", flush=True)
    mon(conn, "C", 0.2)

    for i in range(80):
        time.sleep(0.04)
        blob = mon(conn, "P", 0.9)
        st = parse(blob)
        if not st:
            mon(conn, "C", 0.12)
            continue

        if 0o500 <= st["pc"] <= 0o520 and st["r3"] > 2:
            print(f"  [{i}] clear R3={st['r3']:06o}->1", flush=True)
            mon(conn, "R3=000001", 0.3)
            mon(conn, f"B{TRAP:06o}", 0.2)
            mon(conn, "C", 0.12)
            continue

        nf = read_word(conn, NOFAULT)
        # stacked PC at SP (hw frame)
        stacked_pc = stacked_ps = None
        if st["sp"] < IO_SP and st["sp"] >= 0:
            mb = mon(conn, f"M{st['sp']:06o}", 0.6)
            words = re.findall(rb"\b([0-7]{6})\b", mb)
            # first line words after addr
            if len(words) >= 3:
                # words[0] may be address
                try:
                    stacked_pc = int(words[1], 8)
                    stacked_ps = int(words[2], 8)
                except Exception:
                    pass

        print(
            f"  [{i}] PC={st['pc']:06o} SP={st['sp']:06o} PS={st['ps']:06o} "
            f"nofault={nf if nf is None else f'{nf:06o}'} "
            f"stkPC={stacked_pc and f'{stacked_pc:06o}'} "
            f"stkPS={stacked_ps and f'{stacked_ps:06o}'} "
            f"{st['next']}",
            flush=True,
        )
        for line in blob.decode("latin-1", errors="replace").splitlines():
            if "previous:" in line or "last:" in line:
                print(f"      {line}", flush=True)

        # First real trap: at trap entry, nofault==0
        if st["pc"] == TRAP:
            nf2 = read_word(conn, NOFAULT)
            # Try alternate nofault addresses if 003042 looks wrong
            alts = {}
            for a in (0o003042, 0o003044, 0o001656, 0o001520, 0o001650):
                alts[a] = read_word(conn, a)
            print(f"  nofault alts: " + ", ".join(f"{a:06o}={v and f'{v:06o}'}" for a,v in alts.items()), flush=True)
            mon(conn, "U", 2.0)
            mon(conn, "M000000", 1.0)
            mon(conn, f"M{st['sp']:06o}", 1.0)
            mon(conn, "D000000", 1.0)

            if st["sp"] < 0o1000 or st["sp"] >= IO_SP:
                print("*** SP already emergency/I/O at first seen trap entry", flush=True)
            elif nf2 == 0:
                print("*** FIRST real trap (nofault=0), SP still in RAM", flush=True)
            else:
                print(f"*** trap with nofault={nf2 and f'{nf2:06o}'} (catchfault path)", flush=True)
            # Stop on first trap at 004332 regardless — we need the first one
            break

        mon(conn, f"B{TRAP:06o}", 0.2)
        mon(conn, "C", 0.12)
    else:
        print("no trap seen", flush=True)

    mon(conn, "B clear", 0.3)
    mon(conn, ">", 0.2)
    shell_command(conn, "set break=0", 2.0, True)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
