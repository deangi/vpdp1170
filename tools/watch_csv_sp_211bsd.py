#!/usr/bin/env python3
"""After clear, hit every csv; stop when SP enters I/O. Dump caller."""

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
TRAP = 0o004332
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


def parse_sp_pc(blob: bytes) -> tuple[int, int, int] | None:
    m = re.search(
        rb"state: PC=([0-7]+) .* SP=([0-7]+) PS=([0-7]+)",
        blob,
    )
    if not m:
        m = re.search(
            rb"monitor pause PC=([0-7]+).*?SP=([0-7]+).*?PS=([0-7]+)",
            blob,
        )
    if not m:
        return None
    return int(m.group(1), 8), int(m.group(2), 8), int(m.group(3), 8)


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
    # finish both clears quickly
    mon(conn, "R3=000001", 0.3)
    mon(conn, f"B{CSV:06o}", 0.3)
    mon(conn, f"B{TRAP:06o}", 0.3)  # overwritten — last B wins; re-arm csv in loop
    # Only one breakpoint — use csv
    mon(conn, f"B{CSV:06o}", 0.3)
    print("\n>>> csv watch", flush=True)
    mon(conn, "C", 0.2)

    last_good = None
    for i in range(120):
        time.sleep(0.04)
        blob = mon(conn, "P", 0.85)
        st = parse_sp_pc(blob)
        if not st:
            # maybe still in clear
            r3m = re.search(rb"R3=([0-7]+)", blob)
            pcm = re.search(rb"PC=([0-7]+)", blob)
            if pcm and r3m:
                pc = int(pcm.group(1), 8)
                r3 = int(r3m.group(1), 8)
                if 0o500 <= pc <= 0o520 and r3 > 2:
                    print(f"  [{i}] clear R3={r3:06o}->1", flush=True)
                    mon(conn, "R3=000001", 0.3)
            mon(conn, "C", 0.15)
            continue

        pc, sp, ps = st
        # return addr sits at SP at csv entry? Actually at MOV SP,R5, SP still
        # points at jsr return — word at SP is return PC into caller.
        ret = None
        if pc == CSV:
            mb = mon(conn, f"M{sp:06o}", 0.7)
            mm = re.search(rb"%06o:\s*([0-7]+)" % sp, mb)
            if not mm:
                # format: 003102: xxxxxx or SP addr line
                mm = re.search(rb":\s*([0-7]+)", mb)
            if mm:
                ret = int(mm.group(1), 8)

        mark = "***" if sp >= IO_SP else "   "
        print(
            f"  [{i}]{mark} PC={pc:06o} SP={sp:06o} PS={ps:06o} ret={ret and f'{ret:06o}'}",
            flush=True,
        )

        if sp < IO_SP and pc == CSV:
            last_good = (sp, ret, ps)

        if sp >= IO_SP or pc == TRAP:
            print(f"\n*** BAD SP (last good csv SP/ret={last_good})", flush=True)
            mon(conn, "U", 2.0)
            # full regs
            print(blob.decode("latin-1", errors="replace")[-800:], flush=True)
            # Dump words around SP if in RAM; if I/O, dump physical low + previous
            if last_good and last_good[0] < IO_SP:
                mon(conn, f"M{last_good[0]:06o}", 1.0)
            mon(conn, "M177770", 1.0)
            # previous insn from halt message already in blob
            # Search for who set SP: dump call1 / trap
            mon(conn, "M004000", 1.2)
            mon(conn, "M004332", 1.2)
            break

        # re-arm csv (P clears running; C continues to next hit)
        mon(conn, f"B{CSV:06o}", 0.2)
        mon(conn, "C", 0.15)
    else:
        print("no bad SP in window", flush=True)

    mon(conn, "B clear", 0.3)
    mon(conn, ">", 0.2)
    shell_command(conn, "set break=0", 2.0, True)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
