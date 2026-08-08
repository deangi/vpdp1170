#!/usr/bin/env python3
"""Force clear to finish; poll SP until it enters I/O page or trap hits."""

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
TRAP_PC = 0o004332
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


def mon(conn: TelnetConnection, cmd: str, wait: float = 0.8) -> bytes:
    conn.send(cmd.encode("ascii") + b"\r")
    return drain(conn, wait)


def parse_state(blob: bytes) -> dict:
    matches = list(
        re.finditer(
            rb"state: PC=([0-7]+) R0=([0-7]+) R1=([0-7]+) R2=([0-7]+) "
            rb"R3=([0-7]+) R4=([0-7]+) R5=([0-7]+) SP=([0-7]+) PS=([0-7]+) "
            rb"NEXT=[0-7]+:[0-7]+\s+([^\r\n]+)",
            blob,
        )
    )
    if not matches:
        m = re.search(
            rb"monitor pause PC=([0-7]+).*?SP=([0-7]+).*?PS=([0-7]+)",
            blob,
        )
        if m:
            return {
                "pc": int(m.group(1), 8),
                "sp": int(m.group(2), 8),
                "ps": int(m.group(3), 8),
                "r3": -1,
                "r5": -1,
                "next": "pause",
            }
        return {}
    m = matches[-1]
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


def fmt(st: dict) -> str:
    if not st:
        return "(no state)"
    r3 = f" R3={st['r3']:06o}" if st.get("r3", -1) >= 0 else ""
    r5 = f" R5={st['r5']:06o}" if st.get("r5", -1) >= 0 else ""
    return f"PC={st['pc']:06o} SP={st['sp']:06o} PS={st['ps']:06o}{r3}{r5} {st.get('next','')}"


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
    shell_command(conn, "set break=0", 3.0, True)
    conn.send(b"monitor\r")
    drain(conn, 0.4)
    mon(conn, "B clear", 0.3)
    mon(conn, "C", 0.2)
    mon(conn, ">", 0.2)
    conn.send(b"reset\rexit\r")
    drain(conn, 2.0)

    print("\n--- boot ---", flush=True)
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
    st = parse_state(mon(conn, "P", 1.2))
    print(f"\n*** paused: {fmt(st)}", flush=True)

    # Finish current clear quickly; arm trap; also break on clear loop to
    # patch R3 on the second clear().
    mon(conn, "R3=000001", 0.5)
    mon(conn, "B000506", 0.4)  # clear inner loop — patch 2nd clear too
    print(f"  patched R3: {fmt(parse_state(mon(conn, 'P', 0.8)))}", flush=True)
    mon(conn, "C", 0.3)

    print("\n--- poll SP (P/C bursts) ---", flush=True)
    prev_sp = st.get("sp", 0)
    for i in range(80):
        time.sleep(0.05)
        blob = mon(conn, "P", 0.9)
        st = parse_state(blob)
        if not st:
            mon(conn, "C", 0.2)
            continue
        # If in clear loop with large R3, force finish
        if 0o500 <= st["pc"] <= 0o520 and st.get("r3", 0) > 2:
            print(f"  [{i}] clear again, R3={st['r3']:06o} -> 1", flush=True)
            mon(conn, "R3=000001", 0.4)
            st = parse_state(mon(conn, "P", 0.7))

        if st["sp"] != prev_sp or st["pc"] == TRAP_PC or i % 5 == 0:
            print(f"  [{i}] {fmt(st)}", flush=True)
            prev_sp = st["sp"]

        if st["sp"] >= IO_SP:
            print(f"\n*** SP in I/O page ***", flush=True)
            mon(conn, "U", 2.0)
            mon(conn, f"M{st['pc']:06o}", 1.2)
            mon(conn, f"D{st['sp']:06o}", 1.0)
            # Step a few to see next insns
            for _ in range(5):
                print(f"  step {fmt(parse_state(mon(conn, 'S', 0.5)))}", flush=True)
            break

        if st["pc"] == TRAP_PC:
            print(f"\n*** trap entry, SP={st['sp']:06o} ***", flush=True)
            mon(conn, "U", 2.0)
            mon(conn, f"D{st['sp']:06o}", 1.0)
            # stacked PC
            mon(conn, f"D{(st['sp']):06o}", 1.0)
            break

        mon(conn, "C", 0.2)
    else:
        print("window ended", flush=True)
        print(fmt(parse_state(mon(conn, "P", 0.8))), flush=True)

    mon(conn, "B clear", 0.3)
    mon(conn, ">", 0.2)
    shell_command(conn, "set break=0", 2.0, True)
    conn.close()
    print("\ndone", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
