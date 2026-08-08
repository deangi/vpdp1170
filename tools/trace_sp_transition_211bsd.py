#!/usr/bin/env python3
"""Catch last good csv, then find SP transition before jsr at 052736."""

from __future__ import annotations

import re
import sys
time = __import__("time")
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
BAD_JSR = 0o052736
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


def boot_to_clear(conn: TelnetConnection) -> None:
    profile = BootProfile(
        name="211bsd",
        config_path="/pdpconfig-211bsd.ini",
        completion=b"login: ",
        quiet_seconds=2.0,
    )
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
            return
    raise RuntimeError("no banner")


def main() -> int:
    conn = TelnetConnection(HOST, 23, timeout=2.0)
    conn.connect()
    boot_to_clear(conn)
    ensure_shell(conn)
    conn.send(b"monitor\r")
    drain(conn, 0.4)
    mon(conn, "P", 1.0)
    mon(conn, "R3=000001", 0.3)
    mon(conn, f"B{CSV:06o}", 0.3)
    mon(conn, "C", 0.2)

    # Skip csv hits until we see caller ~042562 a few times, then arm BAD_JSR
    for i in range(30):
        time.sleep(0.04)
        st = parse(mon(conn, "P", 0.85))
        if not st:
            mon(conn, "C", 0.15)
            continue
        if 0o500 <= st["pc"] <= 0o520 and st["r3"] > 2:
            mon(conn, "R3=000001", 0.3)
            mon(conn, "C", 0.15)
            continue
        prev = re.search(
            rb"previous: PC=([0-7]+)",
            mon(conn, "P", 0.1) or b"",
        )
        # re-read with richer blob
        blob = mon(conn, "P", 0.6)
        st = parse(blob)
        pm = re.search(rb"previous: PC=([0-7]+)", blob)
        prev_pc = int(pm.group(1), 8) if pm else -1
        print(
            f"  csv[{i}] SP={st.get('sp',0):06o} prev={prev_pc:06o} "
            f"PS={st.get('ps',0):06o} R0={st.get('r0',0):06o}",
            flush=True,
        )
        if prev_pc == 0o042562 and st.get("sp", 0) < IO_SP:
            # About to leave the autoconfig-ish loop — arm bad jsr
            print("  >>> arm B052736 and poll", flush=True)
            mon(conn, f"B{BAD_JSR:06o}", 0.3)
            mon(conn, "C", 0.2)
            break
        mon(conn, f"B{CSV:06o}", 0.2)
        mon(conn, "C", 0.15)
    else:
        print("never saw 042562 caller", flush=True)
        return 1

    # Wait for 052736 or poll SP
    for j in range(60):
        time.sleep(0.05)
        blob = mon(conn, "P", 0.9)
        st = parse(blob)
        if not st:
            mon(conn, "C", 0.15)
            continue
        print(
            f"  [{j}] PC={st['pc']:06o} SP={st['sp']:06o} PS={st['ps']:06o} "
            f"R0={st['r0']:06o} R1={st['r1']:06o} R5={st['r5']:06o} {st['next']}",
            flush=True,
        )
        if st["pc"] == BAD_JSR or st["sp"] >= IO_SP:
            print("\n*** hit bad site — dump around 052700 and 042540", flush=True)
            mon(conn, "M052700", 1.2)
            mon(conn, "M052720", 1.2)
            mon(conn, "M042540", 1.2)
            mon(conn, "M004370", 1.2)  # near call1/trap
            mon(conn, "M004000", 1.2)
            # 004402 was R0 — dump it
            mon(conn, "M004400", 1.2)
            mon(conn, "U", 2.0)
            # If SP still good somehow, step
            if st["sp"] < IO_SP:
                for _ in range(20):
                    st = parse(mon(conn, "S", 0.45))
                    print(
                        f"  step PC={st.get('pc',0):06o} SP={st.get('sp',0):06o} "
                        f"{st.get('next','')}",
                        flush=True,
                    )
                    if st.get("sp", 0) >= IO_SP:
                        break
            break
        mon(conn, "C", 0.15)
    else:
        print("timeout waiting for 052736", flush=True)

    # Also: from a fresh angle — after good SP, single-step across SPLLOW/trap
    mon(conn, "B clear", 0.3)
    mon(conn, ">", 0.2)
    shell_command(conn, "set break=0", 2.0, True)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
