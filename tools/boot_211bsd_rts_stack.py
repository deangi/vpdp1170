#!/usr/bin/env python3
"""Catch PC=031122, dump (SP) immediately — return addr for RTS."""

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


def drain(conn, seconds: float) -> bytes:
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


def send_line(conn, s: str) -> None:
    conn.send(s.encode("ascii") + b"\r")


def to_shell(conn) -> None:
    send_line(conn, "")
    data = drain(conn, 1.5)
    if b"monitor>" in data:
        send_line(conn, ">")
        drain(conn, 1.5)
        return
    if b"vpdp:" in data or b"management shell" in data:
        return
    # Guest console — Esc into shell. Drain well so '>>' is not left as input.
    print("(Esc>> to shell)", flush=True)
    conn.send(b"\x1b>>")
    data = drain(conn, 4.0)
    if b"monitor>" in data:
        send_line(conn, ">")
        drain(conn, 1.5)
    # Clear any accidental '>>' command line
    send_line(conn, "")
    drain(conn, 0.8)


def to_monitor(conn) -> None:
    to_shell(conn)
    send_line(conn, "monitor")
    data = drain(conn, 2.5)
    if b"monitor>" not in data:
        send_line(conn, "monitor")
        drain(conn, 2.5)
    print("=== monitor ===", flush=True)


def mon(conn, cmd: str, wait: float = 1.5) -> bytes:
    print(f"\n>>> {cmd}", flush=True)
    send_line(conn, cmd)
    return drain(conn, wait)


def guest_cr(conn) -> None:
    print("=== guest CR ===", flush=True)
    mon(conn, "C", 0.3)
    mon(conn, ">", 0.4)
    send_line(conn, "exit")
    drain(conn, 2.0)
    conn.send(b"\r")
    time.sleep(2.0)
    drain(conn, 1.0)
    conn.send(b"\x1b>>")
    drain(conn, 3.0)
    to_monitor(conn)


def parse_state(data: bytes):
    text = data.decode("latin-1", errors="replace")
    pc = sp = None
    m = re.search(r"state: PC=([0-7]+).*?\bSP=([0-7]+)", text)
    if m:
        return m.group(1), m.group(2)
    m1 = re.search(r"\bPC=([0-7]+)", text)
    m2 = re.search(r"\bSP=([0-7]+)", text)
    if m1:
        pc = m1.group(1)
    if m2:
        sp = m2.group(1)
    return pc, sp


def dump_rts_site(conn, buf: bytearray, pc: str, sp: str) -> None:
    print(f"*** AT {pc} SP={sp} — dump stack ***", flush=True)
    spo = int(sp, 8)
    # First word at SP is RTS return PC
    d = mon(conn, f"MD{spo:06o}:{spo + 0o30:06o}", 2.5)
    buf.extend(d)
    # Parse first word
    text = d.decode("latin-1", errors="replace")
    m = re.search(rf"{sp}:\s*([0-7]+)", text)
    if m:
        ret = m.group(1)
        print(f"*** RTS return PC (SP) = {ret} ***", flush=True)
    buf.extend(mon(conn, "U", 3.0))
    buf.extend(mon(conn, "MI031110:031130", 2.0))
    print("=== step RTS ===", flush=True)
    buf.extend(mon(conn, "S", 0.8))
    step = mon(conn, "P", 1.5)
    buf.extend(step)
    step += drain(conn, 0.8)
    buf.extend(mon(conn, "U", 2.5))
    st = step.decode("latin-1", errors="replace")
    if "065054" in st:
        print("*** CONFIRMED PC=065054 after RTS ***", flush=True)


def main() -> int:
    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    log = OUT / f"{stamp}-rts-stack.log"
    buf = bytearray()

    conn = TelnetConnection(HOST, 23, timeout=2.0)
    for i in range(12):
        try:
            conn.connect()
            break
        except Exception as exc:
            print("telnet", i, exc, flush=True)
            time.sleep(2)
            conn = TelnetConnection(HOST, 23, timeout=2.0)

    to_shell(conn)
    print("=== reset ===", flush=True)
    send_line(conn, "reset")
    buf.extend(drain(conn, 8.0))
    # Wait until management shell is definitely back
    for _ in range(10):
        send_line(conn, "")
        data = drain(conn, 1.0)
        buf.extend(data)
        if b"vpdp:" in data:
            break
        time.sleep(1.0)
    to_shell(conn)
    to_monitor(conn)

    data = mon(conn, "P", 2.5)
    buf.extend(data)
    data += drain(conn, 1.0)
    if b"012312" in data or b"177560" in data:
        guest_cr(conn)

    mon(conn, "B clear", 0.4)
    mon(conn, "B031122", 0.5)
    mon(conn, "B", 0.6)
    mon(conn, "C", 0.3)

    print("=== poll until PC=031122 (max 240s) ===", flush=True)
    deadline = time.monotonic() + 240.0
    while time.monotonic() < deadline:
        # Wait for break OR poll every 3s
        hit = bytearray()
        slice_end = time.monotonic() + 3.0
        while time.monotonic() < slice_end:
            try:
                chunk = conn.receive()
            except BenchmarkError:
                chunk = b""
            if chunk:
                hit.extend(chunk)
                sys.stdout.write(chunk.decode("latin-1", errors="replace"))
                sys.stdout.flush()
                if b"031122" in hit:
                    break
            time.sleep(0.05)

        buf.extend(hit)
        data = mon(conn, "P", 1.5)
        buf.extend(data)
        data += drain(conn, 0.5)
        buf.extend(data)
        pc, sp = parse_state(data)
        print(f"poll PC={pc} SP={sp}", flush=True)
        if pc == "031122" and sp:
            dump_rts_site(conn, buf, pc, sp)
            break
        if pc == "031120" and sp:
            # one step to 031122
            mon(conn, "S", 0.6)
            data = mon(conn, "P", 1.2)
            buf.extend(data)
            pc, sp = parse_state(data)
            if pc == "031122" and sp:
                dump_rts_site(conn, buf, pc, sp)
                break
        # still running or elsewhere — continue
        mon(conn, "C", 0.2)
    else:
        print("TIMEOUT no 031122", flush=True)

    mon(conn, "B clear", 0.3)
    mon(conn, ">", 0.4)
    log.write_bytes(buf)
    print(f"log={log}", flush=True)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
