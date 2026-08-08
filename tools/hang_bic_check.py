#!/usr/bin/env python3
"""Board likely paused mid-loop. Dump softc, step through BIC, re-dump."""

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
    SHELL_PROMPT_RE,
    SHELL_BANNER,
)

HOST = "192.168.7.144"
OUT = ROOT / "boot-benchmark-results-211bsd-postphys"

STATE_RE = re.compile(
    r"PC=([0-7]+).*?R0=([0-7]+).*?R1=([0-7]+).*?R3=([0-7]+).*?R4=([0-7]+)"
    r".*?NEXT=[0-7]+:([0-7]+)\s+(\S+(?:\s+\S+){0,6})",
    re.DOTALL,
)


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
    for seq in (b"\x1b>>", b">\r", b"\r"):
        conn.send(seq)
        data = drain(conn, 1.0)
        if SHELL_PROMPT_RE.search(data) or SHELL_BANNER in data:
            conn.send(b"\r")
            drain(conn, 0.4)
            return
    raise RuntimeError("no shell")


def sh(conn, cmd, wait=2.0):
    print(f"\n>>> {cmd}", flush=True)
    conn.send(cmd.encode() + b"\r")
    return drain(conn, wait)


def mon(conn, cmd, wait=1.2):
    print(f"\n>>> mon {cmd}", flush=True)
    conn.send(cmd.encode() + b"\r")
    return drain(conn, wait)


def parse_words(text: str) -> list[str]:
    words = []
    for ln in text.splitlines():
        if re.match(r"^[0-7]{6}:", ln.strip()):
            for p in ln.split(":", 1)[1].split():
                if re.fullmatch(r"[0-7]+", p):
                    words.append(p)
                else:
                    break
    return words


def show_softc(conn, label: str) -> dict:
    data = mon(conn, "D077122:077172", 2.0)
    words = parse_words(data.decode("latin-1", errors="replace"))

    def w(off):
        i = off // 2
        return words[i] if i < len(words) else "?"

    fields = {
        "+6": w(6),
        "+12": w(0o12),
        "+16": w(0o16),
        "+20": w(0o20),
        "+40": w(0o40),
        "+42": w(0o42),
    }
    print(f"\n[{label}] softc " + " ".join(f"{k}={v}" for k, v in fields.items()), flush=True)
    return fields


def main() -> int:
    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    log = OUT / f"{stamp}-bic-check-telnet.log"
    buf = bytearray()

    conn = TelnetConnection(HOST, 23, timeout=2.0)
    conn.connect()
    ensure_shell(conn)

    # Enter monitor; if running, pause at 026532
    buf.extend(sh(conn, "monitor", 0.5))
    buf.extend(mon(conn, "P", 2.0))
    show_softc(conn, "at-entry-pause")

    # Get to 026532 if not there
    buf.extend(mon(conn, "B026532", 0.5))
    buf.extend(mon(conn, "C", 0.3))
    time.sleep(0.5)
    drain(conn, 1.0)
    buf.extend(mon(conn, "P", 1.5))
    fields0 = show_softc(conn, "at-026532")

    print("\n=== step until past BIC pair (or 40 steps) ===\n", flush=True)
    saw_bic = False
    for i in range(45):
        data = mon(conn, "S", 0.35)
        buf.extend(data)
        text = re.sub(r"\s+", " ", data.decode("latin-1", errors="replace"))
        m = STATE_RE.search(text)
        if not m:
            continue
        pc, r0, r1, r3, r4, op, dis = m.groups()
        print(f"S{i+1:02d} PC={pc} R0={r0} R1={r1} R4={r4} {dis}", flush=True)
        if "BIC" in dis:
            saw_bic = True
        if saw_bic and pc == "026532":
            print("*** back at 026532 after BIC ***", flush=True)
            break
        if saw_bic and "026760" in pc:
            # about to BR back — dump softc before branch executes? already at BR
            pass
        if dis.startswith("BR") and "026532" in dis:
            fields_mid = show_softc(conn, "before-BR-to-026532")
            print(f"  +20 before BR = {fields_mid['+20']} (was {fields0['+20']})", flush=True)

    fields1 = show_softc(conn, "after-steps")
    print(
        f"\n+20 before={fields0['+20']} after={fields1['+20']} "
        f"cleared={fields0['+20'] != '000000' and fields1['+20'] == '000000'}",
        flush=True,
    )

    # Dump string at softc+40
    ptr = fields1["+40"]
    if ptr != "?":
        print(f"\n=== dump string @ {ptr} (softc+40) ===", flush=True)
        buf.extend(mon(conn, f"D{ptr}", 1.5))
        # Also try as virtual M
        buf.extend(mon(conn, f"M{ptr}", 1.5))

    buf.extend(mon(conn, "B clear", 0.4))
    buf.extend(mon(conn, ">", 0.4))
    buf.extend(sh(conn, "exit", 0.5))
    conn.close()
    log.write_bytes(buf)
    print(f"\nlog={log}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
