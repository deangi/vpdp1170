#!/usr/bin/env python3
"""Continue from live hang (may already be in monitor). Decode loop + softc+20."""

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
    SHELL_PROMPT_RE,
    SHELL_BANNER,
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


def mon(conn, cmd, wait=1.2) -> bytes:
    print(f"\n>>> mon {cmd}", flush=True)
    conn.send(cmd.encode() + b"\r")
    return drain(conn, wait)


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

    return {f"+{o:o}": w(o) for o in (0, 6, 0o12, 0o16, 0o20, 0o22, 0o24, 0o26, 0o40, 0o42)}


def ensure_monitor(conn: TelnetConnection) -> None:
    """Get to monitor> whether currently in shell, monitor, or guest."""
    data = drain(conn, 0.5)
    text = data.decode("latin-1", errors="replace")
    if "monitor>" in text:
        print("already in monitor", flush=True)
        return
    # try shell first
    try:
        enter_shell(conn, 6.0, True)
        conn.send(b"monitor\r")
        drain(conn, 2.0)
        return
    except Exception:
        pass
    # from guest
    conn.send(b"\x1b>>")
    drain(conn, 1.5)
    enter_shell(conn, 8.0, True)
    conn.send(b"monitor\r")
    drain(conn, 2.0)


def main() -> int:
    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    log = OUT / f"{stamp}-loop-decode2-telnet.log"
    buf = bytearray()

    conn = TelnetConnection(HOST, 23, timeout=2.0)
    conn.connect()
    ensure_monitor(conn)

    for cmd in (
        "P",
        "D077122:077200",
        "M026520:026770",
        "M027200:027260",
        "M144616:144660",
        "D004702:004722",
        "D140534:140600",
        "D012774:013020",
        "D014110",
        "M014110",
    ):
        buf.extend(mon(conn, cmd, 2.0))

    sc = mon(conn, "D077122:077200", 2.0)
    buf.extend(sc)
    fields = softc_fields(parse_words(sc.decode("latin-1", errors="replace")))
    print(f"\nsoftc: {fields}", flush=True)

    # Track softc+16/+20 across loop-top hits
    buf.extend(mon(conn, "B026532", 0.5))
    hits = []
    for i in range(10):
        buf.extend(mon(conn, "C", 0.3))
        time.sleep(0.35)
        drain(conn, 0.6)
        buf.extend(mon(conn, "P", 1.2))
        sc2 = mon(conn, "D077122:077166", 1.5)
        buf.extend(sc2)
        f = softc_fields(parse_words(sc2.decode("latin-1", errors="replace")))
        hits.append(f)
        print(
            f"hit{i+1}: +16={f.get('+16')} +20={f.get('+20')} "
            f"+12={f.get('+12')} +40={f.get('+40')} +42={f.get('+42')}",
            flush=True,
        )

    # Step one iteration watching BIC effect
    print("\n=== one iteration steps ===\n", flush=True)
    for i in range(35):
        data = mon(conn, "S", 0.3)
        buf.extend(data)
        text = re.sub(r"\s+", " ", data.decode("latin-1", errors="replace"))
        m = re.search(
            r"PC=([0-7]+).*?R0=([0-7]+).*?R1=([0-7]+).*?NEXT=[0-7]+:([0-7]+)\s+(\S+(?:\s+\S+){0,6})",
            text,
        )
        if not m:
            continue
        pc, r0, r1, op, dis = m.groups()
        print(f"S{i+1:02d} PC={pc} R0={r0} R1={r1} {dis}", flush=True)
        if "BIC" in dis and "20(R4)" in dis.replace(" ", ""):
            sc3 = mon(conn, "D077142:077146", 1.0)
            buf.extend(sc3)
            print(f"  after BIC-ish dump: {sc3.decode('latin-1','replace')[:120]}", flush=True)
        if pc == "026532" and i > 5:
            sc4 = mon(conn, "D077122:077166", 1.5)
            buf.extend(sc4)
            f = softc_fields(parse_words(sc4.decode("latin-1", errors="replace")))
            print(f"  back at loop: +16={f.get('+16')} +20={f.get('+20')}", flush=True)
            break

    buf.extend(mon(conn, "B clear", 0.4))
    buf.extend(mon(conn, ">", 0.5))
    try:
        shell_command(conn, "exit", 3.0, True)
    except BenchmarkError:
        conn.send(b"exit\r")
        drain(conn, 1.0)
    conn.close()
    log.write_bytes(buf)
    print(f"\nlog={log}", flush=True)
    print("hits summary:", hits, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
