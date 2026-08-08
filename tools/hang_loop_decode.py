#!/usr/bin/env python3
"""Decode 026532 loop; check softc+20 across iterations; dump jump table."""

from __future__ import annotations

import re
import sys
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from benchmark_boot_times import (  # noqa: E402
    TelnetConnection,
    BenchmarkError,
    enter_shell,
    shell_command,
)

HOST = "192.168.7.144"
OUT = ROOT / "boot-benchmark-results-211bsd-postphys"


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


def mon(conn, cmd, wait=1.0):
    print(f"\n>>> mon {cmd}", flush=True)
    conn.send(cmd.encode() + b"\r")
    return drain(conn, wait)


def boot_to_hang(conn: TelnetConnection) -> bool:
    """Reset already done; on console — feed CR until user mem, then wait for hang."""
    start = time.monotonic()
    last_cr = 0.0
    cr_n = 0
    user_at = None
    last_out = start
    buf = bytearray()
    while time.monotonic() < start + 180:
        now = time.monotonic()
        if (
            cr_n < 25
            and now - start < 60
            and b"2.11 BSD" not in buf
            and now - last_cr >= 2.0
        ):
            conn.send(b"\r")
            last_cr = now
            cr_n += 1
        try:
            chunk = conn.receive()
        except BenchmarkError:
            time.sleep(0.05)
            continue
        if chunk:
            buf.extend(chunk)
            last_out = now
            sys.stdout.write(chunk.decode("latin-1", errors="replace"))
            sys.stdout.flush()
            if b"user mem" in buf and user_at is None:
                user_at = now
                print("\n*** user mem ***\n", flush=True)
            if b"configure system" in buf or b"login: " in buf:
                print("booted further than hang!", flush=True)
                return True
            continue
        if user_at and now - last_out > 25:
            print("\n*** hang ***\n", flush=True)
            return True
    return False


def parse_words(text: str) -> list[str]:
    words = []
    for ln in text.splitlines():
        s = ln.strip()
        if re.match(r"^[0-7]{6}:", s):
            for p in s.split(":", 1)[1].split():
                if re.fullmatch(r"[0-7]+", p):
                    words.append(p)
                else:
                    break
    return words


def softc_fields(words: list[str]) -> dict:
    def w(off_oct: int) -> str:
        i = off_oct // 2
        return words[i] if i < len(words) else "?"

    return {
        "+0": w(0),
        "+6": w(0o6),
        "+12": w(0o12),
        "+16": w(0o16),
        "+20": w(0o20),
        "+22": w(0o22),
        "+24": w(0o24),
        "+26": w(0o26),
        "+40": w(0o40),
        "+42": w(0o42),
    }


def main() -> int:
    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    log = OUT / f"{stamp}-loop-decode-telnet.log"
    buf = bytearray()

    conn = TelnetConnection(HOST, 23, timeout=2.0)
    conn.connect()
    enter_shell(conn, 10.0, True)

    # Fresh boot into hang
    shell_command(conn, "rm /pdpconfig.ini", 5.0, True)
    shell_command(conn, "cp /pdpconfig-211bsd.ini /pdpconfig.ini", 5.0, True)
    shell_command(conn, "set pcping=0", 5.0, True)
    shell_command(conn, "reset", 5.0, True)
    # exit returns to guest console — do not wait for shell prompt
    conn.send(b"exit\r")
    drain(conn, 1.0)
    print("\n=== boot to hang (CR feed) ===\n", flush=True)
    if not boot_to_hang(conn):
        print("TIMEOUT waiting for hang", flush=True)
        conn.close()
        return 1
    print("\n*** enter shell ***\n", flush=True)
    enter_shell(conn, 10.0, True)

    buf.extend(shell_command(conn, "rl regs", 5.0, True))
    buf.extend(shell_command(conn, "monitor", 3.0, True))

    # Dump loop body + softc + jump tables
    for cmd in (
        "P",
        "D077122:077200",
        "M026500:026770",
        "M027200:027260",
        "M144616:144660",
        "D014110",
        "M014110",
        "D004702:004720",
        "D140534:140600",
    ):
        buf.extend(mon(conn, cmd, 2.0))

    fields = softc_fields(
        parse_words(
            # re-dump softc cleanly
            mon(conn, "D077122:077200", 2.0).decode("latin-1", errors="replace")
        )
    )
    print(f"\nsoftc fields: {fields}", flush=True)

    # Break at loop top; capture softc+20 over 8 hits
    buf.extend(mon(conn, "B026532", 0.5))
    hits = []
    for i in range(8):
        buf.extend(mon(conn, "C", 0.3))
        time.sleep(0.4)
        drain(conn, 0.8)
        data = mon(conn, "P", 1.5)
        buf.extend(data)
        # dump +16/+20 only
        sc = mon(conn, "D077142:077146", 1.0)  # +20 area at 077142
        buf.extend(sc)
        words = parse_words(sc.decode("latin-1", errors="replace"))
        # Also full softc snapshot of key fields
        sc2 = mon(conn, "D077122:077166", 1.5)
        buf.extend(sc2)
        f = softc_fields(parse_words(sc2.decode("latin-1", errors="replace")))
        hits.append(f)
        print(f"hit{i+1}: +16={f['+16']} +20={f['+20']} +12={f['+12']} +42={f['+42']}", flush=True)

    # Single-step from 026532 when +20 is nonzero (or anyway) until JSR or exit
    print("\n=== step from 026532 until JSR 144616 or BR out ===\n", flush=True)
    for i in range(30):
        data = mon(conn, "S", 0.35)
        buf.extend(data)
        text = re.sub(r"\s+", " ", data.decode("latin-1", errors="replace"))
        m = re.search(
            r"PC=([0-7]+).*?R0=([0-7]+).*?R1=([0-7]+).*?NEXT=[0-7]+:([0-7]+)\s+(\S+(?:\s+\S+){0,5})",
            text,
        )
        if m:
            pc, r0, r1, op, dis = m.groups()
            print(f"S{i+1:02d} PC={pc} R0={r0} R1={r1} {dis}", flush=True)
            if "144616" in dis or pc == "144616":
                print("*** entered ffs ***", flush=True)
                break
            if pc.startswith("03") or pc.startswith("05") or pc.startswith("00"):
                print("*** left 026xxx region ***", flush=True)
                break

    buf.extend(mon(conn, "B clear", 0.4))
    buf.extend(mon(conn, ">", 0.4))
    buf.extend(shell_command(conn, "exit", 5.0, True))
    conn.close()
    log.write_bytes(buf)
    print(f"\nlog={log}", flush=True)
    print("hits:", hits, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
