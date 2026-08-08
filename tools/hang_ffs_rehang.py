#!/usr/bin/env python3
"""Boot to hang (robust), dump 144616, break for R1."""

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
MAX_HITS = 12

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
        data = drain(conn, 1.0)
        if SHELL_PROMPT_RE.search(data) or SHELL_BANNER in data:
            conn.send(b"\r")
            drain(conn, 0.4)
            return
    raise RuntimeError("no shell")


def sh(conn: TelnetConnection, cmd: str, wait: float = 2.0) -> bytes:
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
            if not data:
                continue
            f.write(data)
            f.flush()
            buf += data.decode("latin-1", errors="replace")
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                m = BREAK_RE.search(line.rstrip("\r"))
                if m and m.group(1) == BP:
                    hit = {
                        "ps": m.group(2),
                        "r0": m.group(3),
                        "r1": m.group(4),
                        "r3": m.group(6),
                        "r4": m.group(7),
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
    serial_log = OUT / f"{stamp}-ffs2-com18.log"
    telnet_log = OUT / f"{stamp}-ffs2-telnet.log"
    hits: list = []

    stop = threading.Event()
    thr = threading.Thread(
        target=serial_reader, args=(stop, serial_log, hits), daemon=True
    )
    thr.start()
    time.sleep(0.5)

    conn = TelnetConnection(HOST, 23, timeout=2.0)
    conn.connect()
    ensure_shell(conn)
    buf = bytearray()

    for cmd, w in (
        ("rm /pdpconfig.ini", 3.0),
        ("cp /pdpconfig-211bsd.ini /pdpconfig.ini", 3.0),
        ("set pcping=0", 1.5),
    ):
        buf.extend(sh(conn, cmd, w))

    buf.extend(sh(conn, "reset", 1.0))
    time.sleep(0.3)
    buf.extend(sh(conn, "exit", 1.0))

    start = time.monotonic()
    last_out = start
    last_cr = 0.0
    cr_n = 0
    user_at = None
    while time.monotonic() < start + 200:
        now = time.monotonic()
        if cr_n < 20 and now - start < 50 and b"2.11 BSD" not in buf and now - last_cr >= 2.0:
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
                break
            continue
        if user_at and now - last_out > 30:
            print("\n*** hang ***\n", flush=True)
            break

    ensure_shell(conn)
    print("=== dump ===", flush=True)
    buf.extend(sh(conn, "rl regs", 2.0))
    buf.extend(sh(conn, "monitor", 0.6))
    buf.extend(mon(conn, "P", 2.0))
    for cmd, w in (("M144600", 1.5), ("M144616", 1.5), ("M026640", 1.5), ("D077122", 1.2)):
        buf.extend(mon(conn, cmd, w))

    print(f"\n=== break {BP} ===\n", flush=True)
    buf.extend(mon(conn, f"B{BP}", 0.5))
    buf.extend(mon(conn, "C", 0.3))

    last_n = 0
    end = time.monotonic() + 35
    while time.monotonic() < end and len(hits) < MAX_HITS:
        t0 = time.monotonic()
        while time.monotonic() < t0 + 2.0 and len(hits) == last_n:
            time.sleep(0.05)
        if len(hits) == last_n:
            continue
        last_n = len(hits)
        time.sleep(0.1)
        buf.extend(mon(conn, "C", 0.25))

    buf.extend(mon(conn, "P", 1.5))
    buf.extend(mon(conn, "B clear", 0.4))
    buf.extend(mon(conn, ">", 0.4))
    buf.extend(sh(conn, "exit", 0.5))

    stop.set()
    thr.join(timeout=2)
    conn.close()
    telnet_log.write_bytes(buf)

    print("\n=== R1 summary ===", flush=True)
    print(f"hits={len(hits)}", flush=True)
    if hits:
        r1s = [h["r1"] for h in hits]
        print("unique R1", sorted(set(r1s)), flush=True)
        print("R1==0", sum(1 for r in r1s if int(r, 8) == 0), "/", len(hits), flush=True)
        for i, h in enumerate(hits, 1):
            print(i, h, flush=True)

    text = buf.decode("latin-1", errors="replace")
    print("\n=== dumps of interest ===", flush=True)
    for ln in text.splitlines():
        if any(
            k in ln
            for k in (
                "1446",
                "0266",
                "CSR=",
                "state: PC=",
                "phys mem",
                "user mem",
                "configure",
            )
        ):
            print(ln, flush=True)

    print(f"\ntelnet={telnet_log}\ncom18={serial_log}", flush=True)
    return 0 if hits else 1


if __name__ == "__main__":
    raise SystemExit(main())
