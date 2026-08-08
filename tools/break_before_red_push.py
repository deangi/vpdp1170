#!/usr/bin/env python3
"""Break at 165502 (MOV R0,-(SP) before red) and dump SP."""

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
TARGET = 0o165502  # MOV R0,-(SP) that likely triggers red


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
    # Arm break on the fatal push — kernel VA valid after overlays mapped
    mon(conn, f"B{TARGET:06o}", 0.4)
    mon(conn, "B000506", 0.3)  # oops only one break — use TARGET only
    mon(conn, f"B{TARGET:06o}", 0.4)
    print(f"\n>>> waiting for PC={TARGET:06o}", flush=True)
    mon(conn, "C", 0.3)

    for i in range(50):
        time.sleep(0.08)
        blob = mon(conn, "P", 1.0)
        st = parse(blob)
        if not st:
            if re.search(rb"PC=0005", blob):
                mon(conn, "R3=000001", 0.3)
                mon(conn, f"B{TARGET:06o}", 0.3)
            mon(conn, "C", 0.15)
            continue
        print(
            f"  [{i}] PC={st['pc']:06o} SP={st['sp']:06o} R5={st['r5']:06o} "
            f"R0={st['r0']:06o} R4={st['r4']:06o} {st['next']}",
            flush=True,
        )
        if st["pc"] == TARGET or st["sp"] < 0o4000 or st["sp"] >= 0o160000:
            print("\n*** at target / interesting SP ***", flush=True)
            mon(conn, "U", 2.0)
            mon(conn, "M177774", 1.0)
            mon(conn, f"M{st['sp']:06o}", 1.2)
            # eintstk / intstk region
            mon(conn, "M002000", 1.0)
            mon(conn, "M003000", 1.0)
            # step the push and see
            print(">>> step push", flush=True)
            for s in range(5):
                st2 = parse(mon(conn, "S", 0.5))
                print(
                    f"  step[{s}] PC={st2.get('pc',0):06o} SP={st2.get('sp',0):06o} "
                    f"CPU? {st2.get('next','')}",
                    flush=True,
                )
                if st2.get("sp", 0) == 4 or st2.get("sp", 0) == 0:
                    mon(conn, "U", 2.0)
                    mon(conn, "D000000", 1.0)
                    break
            break
        mon(conn, f"B{TARGET:06o}", 0.2)
        mon(conn, "C", 0.15)
    else:
        print("never hit", flush=True)

    mon(conn, "B clear", 0.3)
    mon(conn, ">", 0.2)
    shell_command(conn, "set break=0", 2.0, True)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
