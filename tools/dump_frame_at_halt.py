#!/usr/bin/env python3
"""After vector-4 HALT: dump R5 frame (good stack) and disassemble 042540-042650."""

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

    # Wait until past clear: break csv once from 042562, THEN plant HALT vector
    mon(conn, "B003102", 0.3)
    mon(conn, "C", 0.2)
    for i in range(20):
        time.sleep(0.05)
        blob = mon(conn, "P", 0.85)
        st = parse(blob)
        if not st:
            if re.search(rb"PC=0005", blob):
                mon(conn, "R3=000001", 0.3)
            mon(conn, "C", 0.12)
            continue
        pm = re.search(rb"previous: PC=([0-7]+)", blob)
        prev = int(pm.group(1), 8) if pm else -1
        print(f"  csv SP={st['sp']:06o} prev={prev:06o}", flush=True)
        if prev == 0o042562 and st["sp"] < 0o160000:
            print("  >>> plant HALT vector now", flush=True)
            mon(conn, "W001000=000000", 0.4)
            mon(conn, "W000004=001000", 0.4)
            mon(conn, "W000006=000340", 0.4)
            mon(conn, "B clear", 0.3)
            mon(conn, "C", 0.2)
            break
        mon(conn, "B003102", 0.2)
        mon(conn, "C", 0.12)
    else:
        print("no 042562", flush=True)
        return 1

    for j in range(30):
        time.sleep(0.06)
        blob = mon(conn, "P", 1.0)
        st = parse(blob)
        if not st:
            mon(conn, "C", 0.12)
            continue
        print(
            f"  [{j}] PC={st['pc']:06o} SP={st['sp']:06o} R5={st['r5']:06o} "
            f"R1={st['r1']:06o} R2={st['r2']:06o} PS={st['ps']:06o}",
            flush=True,
        )
        if st["pc"] == 0o1000:
            r5 = st["r5"]
            print("\n*** HALT — dump R5 frame and code", flush=True)
            mon(conn, "U", 2.0)
            # C frame at R5: old r5, ovno, r4, r3, r2 below; return above
            mon(conn, f"M{r5:06o}", 1.2)
            mon(conn, f"M{(r5-0o20) & 0o177777:06o}", 1.2)
            mon(conn, f"M{(r5+0o20) & 0o177777:06o}", 1.2)
            # Also physical for u-area
            # KDSA6=004605 → phys = 00460500 + (r5 - 0140000)
            # r5=147540 → offset from page6 = 147540 - 140000 = 7540
            # phys = 00460500 + 07540 = 00470240
            mon(conn, "D0470240", 1.2)
            mon(conn, "M042520", 1.3)
            mon(conn, "M042540", 1.3)
            mon(conn, "M042560", 1.3)
            mon(conn, "M042600", 1.3)
            mon(conn, "M042620", 1.3)
            mon(conn, "M042640", 1.3)
            # stacked words at SP (I/O — may be junk); low mem 0-10
            mon(conn, "D000000", 1.2)
            # CPUERR
            print("CPUERR via U above; PIR note", flush=True)
            break
        mon(conn, "C", 0.12)

    mon(conn, "B clear", 0.3)
    mon(conn, ">", 0.2)
    shell_command(conn, "set break=0", 2.0, True)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
