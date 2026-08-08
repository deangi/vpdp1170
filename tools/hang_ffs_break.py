#!/usr/bin/env python3
"""Dump 144616 bit-scan; break on entry; record R1 from kek BREAK lines."""

from __future__ import annotations

import re
import sys
import threading
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
COM = "COM18"
OUT = ROOT / "boot-benchmark-results-211bsd-postphys"
BP = "144616"
MAX_HITS = 15

BREAK_RE = re.compile(
    r"kek BREAK pc=([0-7]+) PS=([0-7]+) "
    r"R0=([0-7]+) R1=([0-7]+) R2=([0-7]+) R3=([0-7]+) "
    r"R4=([0-7]+) R5=([0-7]+) SP=([0-7]+)"
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
        data = drain(conn, 0.8)
        if SHELL_PROMPT_RE.search(data) or SHELL_BANNER in data:
            conn.send(b"\r")
            drain(conn, 0.3)
            return
    raise RuntimeError("no shell")


def sh(conn: TelnetConnection, cmd: str, wait: float = 1.5) -> bytes:
    print(f"\n>>> {cmd}", flush=True)
    conn.send(cmd.encode("ascii") + b"\r")
    return drain(conn, wait)


def mon(conn: TelnetConnection, cmd: str, wait: float = 1.2) -> bytes:
    print(f"\n>>> mon {cmd}", flush=True)
    conn.send(cmd.encode("ascii") + b"\r")
    return drain(conn, wait)


def serial_reader(stop: threading.Event, log_path: Path, hits: list) -> None:
    import serial

    ser = serial.Serial(COM, 115200, timeout=0.2)
    with log_path.open("wb") as f:
        buf = ""
        while not stop.is_set():
            data = ser.read(8192)
            if data:
                f.write(data)
                f.flush()
                buf += data.decode("latin-1", errors="replace")
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    line = line.rstrip("\r")
                    m = BREAK_RE.search(line)
                    if m and m.group(1) == BP:
                        hit = {
                            "pc": m.group(1),
                            "ps": m.group(2),
                            "r0": m.group(3),
                            "r1": m.group(4),
                            "r2": m.group(5),
                            "r3": m.group(6),
                            "r4": m.group(7),
                            "r5": m.group(8),
                            "sp": m.group(9),
                        }
                        hits.append(hit)
                        print(
                            f"HIT#{len(hits)} R1={hit['r1']} R0={hit['r0']} "
                            f"R3={hit['r3']} R4={hit['r4']} PS={hit['ps']}",
                            flush=True,
                        )
    ser.close()


def main() -> int:
    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    serial_log = OUT / f"{stamp}-ffs-break-com18.log"
    telnet_log = OUT / f"{stamp}-ffs-break-telnet.log"
    hits: list = []

    stop = threading.Event()
    thr = threading.Thread(
        target=serial_reader, args=(stop, serial_log, hits), daemon=True
    )
    thr.start()
    time.sleep(0.4)

    conn = TelnetConnection(HOST, 23, timeout=2.0)
    conn.connect()
    ensure_shell(conn)
    telnet_buf = bytearray()

    def cap(data: bytes) -> None:
        telnet_buf.extend(data)

    print("=== dump bit-scan routine + caller ===", flush=True)
    cap(sh(conn, "rl regs", 2.0))
    cap(sh(conn, "monitor", 0.6))
    cap(mon(conn, "P", 2.0))
    for cmd, w in (
        ("M144600", 1.5),
        ("M144616", 1.5),
        ("M026640", 1.5),
        ("D077120", 1.2),
    ):
        cap(mon(conn, cmd, w))

    print(f"\n=== B{BP} + catch hits ===\n", flush=True)
    cap(mon(conn, f"B{BP}", 0.6))
    cap(mon(conn, "C", 0.3))

    deadline = time.monotonic() + 40.0
    last_n = 0
    while time.monotonic() < deadline and len(hits) < MAX_HITS:
        # Wait until a new break hit appears on serial
        wait_end = time.monotonic() + 2.5
        while time.monotonic() < wait_end and len(hits) == last_n:
            time.sleep(0.05)
        if len(hits) == last_n:
            continue
        last_n = len(hits)
        # CPU is paused at breakpoint; continue for next hit
        time.sleep(0.15)
        cap(mon(conn, "C", 0.25))

    # Stop
    cap(mon(conn, "P", 1.5))
    cap(mon(conn, "B clear", 0.5))
    cap(mon(conn, ">", 0.4))
    cap(sh(conn, "rl regs", 2.0))
    cap(sh(conn, "exit", 0.5))

    stop.set()
    thr.join(timeout=2)
    conn.close()
    telnet_log.write_bytes(telnet_buf)

    print("\n=== R1 on entry to 144616 ===", flush=True)
    r1s = [h["r1"] for h in hits]
    print(f"hits={len(hits)} unique R1={sorted(set(r1s))}", flush=True)
    zeros = sum(1 for r in r1s if int(r, 8) == 0)
    print(f"R1==0 count: {zeros}/{len(hits)}", flush=True)
    for i, h in enumerate(hits, 1):
        print(
            f"  #{i} R1={h['r1']} R0={h['r0']} R4={h['r4']} R3={h['r3']} PS={h['ps']}",
            flush=True,
        )

    print(f"\ntelnet={telnet_log}\ncom18={serial_log}", flush=True)
    return 0 if hits else 1


if __name__ == "__main__":
    raise SystemExit(main())
