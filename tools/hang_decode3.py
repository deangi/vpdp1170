#!/usr/bin/env python3
"""Direct monitor session: dump + softc+20 hits. Assumes board hung; gets to monitor>."""

from __future__ import annotations

import re
import sys
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from benchmark_boot_times import TelnetConnection, BenchmarkError  # noqa: E402

HOST = "192.168.7.144"
OUT = ROOT / "boot-benchmark-results-211bsd-postphys"


def drain(conn, seconds):
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


def mon(conn, cmd, wait=1.5):
    print(f"\n>>> {cmd}", flush=True)
    conn.send(cmd.encode() + b"\r")
    return drain(conn, wait)


def parse_words(text):
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


def fields(words):
    def w(o):
        i = o // 2
        return words[i] if i < len(words) else "?"

    return {f"+{o:o}": w(o) for o in (0, 6, 0o12, 0o16, 0o20, 0o22, 0o24, 0o40, 0o42)}


def to_monitor(conn):
    data = drain(conn, 0.8)
    if b"monitor>" in data:
        return
    # leave monitor to shell
    if b"monitor>" in data or True:
        conn.send(b">\r")
        data = drain(conn, 1.0)
    if b"vpdp:/>" in data or b"management shell" in data:
        conn.send(b"monitor\r")
        drain(conn, 1.5)
        return
    # guest -> shell -> monitor
    conn.send(b"\x1b>>")
    data = drain(conn, 2.0)
    if b"vpdp:/>" in data or b"management shell" in data:
        conn.send(b"monitor\r")
        drain(conn, 1.5)
        return
    # maybe already monitor after >
    conn.send(b"\r")
    data = drain(conn, 0.8)
    if b"monitor>" in data:
        return
    raise RuntimeError("could not reach monitor")


def main():
    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    log = OUT / f"{stamp}-decode3-telnet.log"
    buf = bytearray()

    conn = TelnetConnection(HOST, 23, timeout=2.0)
    conn.connect()
    # Probe
    conn.send(b"\r")
    data = drain(conn, 1.0)
    buf.extend(data)
    if b"monitor>" in data:
        print("in monitor", flush=True)
    elif b"vpdp:/>" in data:
        print("in shell -> monitor", flush=True)
        buf.extend(mon(conn, "monitor", 2.0))
    else:
        print("try escape to shell", flush=True)
        conn.send(b"\x1b>>")
        data = drain(conn, 2.5)
        buf.extend(data)
        if b"management shell" in data or b"vpdp:/>" in data:
            buf.extend(mon(conn, "monitor", 2.0))
        elif b"monitor>" in data:
            pass
        else:
            # leftover monitor from prior run — send ? 
            conn.send(b"?\r")
            data = drain(conn, 1.5)
            buf.extend(data)

    for cmd in (
        "P",
        "D077122:077200",
        "M026520:026770",
        "M027200:027260",
        "M144616:144660",
        "D004702:004722",
        "D140534:140600",
        "D012774:013020",
        "M014110",
    ):
        buf.extend(mon(conn, cmd, 2.0))

    sc = mon(conn, "D077122:077200", 2.0)
    buf.extend(sc)
    f0 = fields(parse_words(sc.decode("latin-1", errors="replace")))
    print(f"\nsoftc0: {f0}", flush=True)

    buf.extend(mon(conn, "B026532", 0.5))
    hits = []
    for i in range(8):
        buf.extend(mon(conn, "C", 0.3))
        time.sleep(0.4)
        drain(conn, 0.5)
        buf.extend(mon(conn, "P", 1.2))
        sc2 = mon(conn, "D077122:077166", 1.5)
        buf.extend(sc2)
        f = fields(parse_words(sc2.decode("latin-1", errors="replace")))
        hits.append(f)
        print(
            f"hit{i+1}: +16={f.get('+16')} +20={f.get('+20')} +40={f.get('+40')} +42={f.get('+42')}",
            flush=True,
        )

    print("\n=== steps ===\n", flush=True)
    for i in range(40):
        data = mon(conn, "S", 0.3)
        buf.extend(data)
        text = re.sub(r"\s+", " ", data.decode("latin-1", errors="replace"))
        m = re.search(
            r"PC=([0-7]+).*?R0=([0-7]+).*?R1=([0-7]+).*?NEXT=[0-7]+:([0-7]+)\s+(\S+(?:\s+\S+){0,6})",
            text,
        )
        if m:
            pc, r0, r1, op, dis = m.groups()
            print(f"S{i+1:02d} PC={pc} R0={r0} R1={r1} {dis}", flush=True)
            if pc == "026532" and i > 3:
                sc3 = mon(conn, "D077122:077166", 1.5)
                buf.extend(sc3)
                f = fields(parse_words(sc3.decode("latin-1", errors="replace")))
                print(f"  reentry +16={f.get('+16')} +20={f.get('+20')}", flush=True)
                break

    buf.extend(mon(conn, "B clear", 0.4))
    log.write_bytes(buf)
    print(f"\nlog={log}\nhits={hits}", flush=True)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
