#!/usr/bin/env python3
"""Break at STCFI F2,-(SP) @031112; dump SP/stack before & after through RTS.

Sequence at site:
  031110: 173201  SUBF F1,F2
  031112: 175646  STCFI F2,-(SP)   # FL decides 1 vs 2 words
  031114: 012600  MOV (SP)+,R0
  031116: 012601  MOV (SP)+,R1
  031120: 170002  SETI
  031122: 000207  RTS
If FL=0 (short), second MOV steals the return PC and RTS pops junk.
"""

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
BREAK_PC = "031112"


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
    print("(Esc>> to shell)", flush=True)
    conn.send(b"\x1b>>")
    data = drain(conn, 4.0)
    if b"monitor>" in data:
        send_line(conn, ">")
        drain(conn, 1.5)
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
    pc = sp = ps = r0 = r1 = None
    m = re.search(
        r"state: PC=([0-7]+).*?\bR0=([0-7]+).*?\bR1=([0-7]+).*?\bSP=([0-7]+).*?\bPS=([0-7]+)",
        text,
        re.S,
    )
    if m:
        return m.group(1), m.group(4), m.group(5), m.group(2), m.group(3)
    m1 = re.search(r"\bPC=([0-7]+)", text)
    m2 = re.search(r"\bSP=([0-7]+)", text)
    m3 = re.search(r"\bPS=([0-7]+)", text)
    m4 = re.search(r"\bR0=([0-7]+)", text)
    m5 = re.search(r"\bR1=([0-7]+)", text)
    if m1:
        pc = m1.group(1)
    if m2:
        sp = m2.group(1)
    if m3:
        ps = m3.group(1)
    if m4:
        r0 = m4.group(1)
    if m5:
        r1 = m5.group(1)
    return pc, sp, ps, r0, r1


def stack_words(dump: bytes, sp: str, n: int = 8) -> list[str]:
    text = dump.decode("latin-1", errors="replace")
    spo = int(sp, 8)
    words = []
    # M dump lines look like: 175310: 000001 000002 ...
    for line in text.splitlines():
        m = re.match(r"^([0-7]+):\s+((?:[0-7]+\s*)+)", line.strip())
        if not m:
            continue
        base = int(m.group(1), 8)
        vals = m.group(2).split()
        for i, v in enumerate(vals):
            addr = base + i * 2
            if spo <= addr < spo + n * 2:
                words.append((f"{addr:06o}", v))
    return words


def summarize(label: str, pc, sp, r0, r1, words) -> None:
    print(f"*** {label}: PC={pc} SP={sp} R0={r0} R1={r1}", flush=True)
    if words:
        print("*** stack@", flush=True)
        for a, v in words:
            mark = " <--SP" if a == sp else ""
            print(f"    {a}: {v}{mark}", flush=True)


def dump_around_sp(conn, buf: bytearray, sp: str) -> list:
    spo = int(sp, 8)
    # include a few words below SP (higher addrs) for return-slot visibility
    lo = max(0, spo - 0o10)
    hi = spo + 0o20
    d = mon(conn, f"MD{lo:06o}:{hi:06o}", 2.5)
    buf.extend(d)
    return stack_words(d, f"{lo:06o}", n=20)


def main() -> int:
    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    log = OUT / f"{stamp}-stcfi-stack.log"
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
    mon(conn, f"B{BREAK_PC}", 0.5)
    mon(conn, "B", 0.6)
    mon(conn, "C", 0.3)

    print(f"=== poll until PC={BREAK_PC} (max 240s) ===", flush=True)
    deadline = time.monotonic() + 240.0
    hit_ok = False
    while time.monotonic() < deadline:
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
                if BREAK_PC.encode() in hit:
                    break
            time.sleep(0.05)

        buf.extend(hit)
        data = mon(conn, "P", 1.5)
        buf.extend(data)
        data += drain(conn, 0.5)
        buf.extend(data)
        pc, sp, ps, r0, r1 = parse_state(data)
        print(f"poll PC={pc} SP={sp} PS={ps}", flush=True)
        if pc == BREAK_PC and sp:
            hit_ok = True
            break
        if pc == "031110" and sp:
            mon(conn, "S", 0.6)
            data = mon(conn, "P", 1.2)
            buf.extend(data)
            pc, sp, ps, r0, r1 = parse_state(data)
            if pc == BREAK_PC and sp:
                hit_ok = True
                break
        mon(conn, "C", 0.2)

    if not hit_ok:
        print("TIMEOUT no", BREAK_PC, flush=True)
        mon(conn, "B clear", 0.3)
        mon(conn, ">", 0.4)
        log.write_bytes(buf)
        print(f"log={log}", flush=True)
        conn.close()
        return 1

    # --- before STCFI ---
    buf.extend(mon(conn, "U", 3.0))
    words = dump_around_sp(conn, buf, sp)
    summarize("BEFORE STCFI", pc, sp, r0, r1, words)
    sp_before = int(sp, 8)
    # word at SP = putative return PC for eventual RTS (if no locals below)
    ret_slot = None
    for a, v in words:
        if a == sp:
            ret_slot = v
            break
    print(f"*** word@SP (pre) = {ret_slot}", flush=True)

    # step STCFI
    print("=== step STCFI ===", flush=True)
    buf.extend(mon(conn, "S", 0.8))
    data = mon(conn, "P", 1.5)
    buf.extend(data)
    pc, sp, ps, r0, r1 = parse_state(data)
    words = dump_around_sp(conn, buf, sp if sp else f"{sp_before:06o}")
    summarize("AFTER STCFI", pc, sp, r0, r1, words)
    if sp:
        delta = int(sp, 8) - sp_before
        print(f"*** SP delta = {delta} (expect -4 if SETL/long, -2 if SETI/short)", flush=True)
        if delta == -2:
            print("*** SHORT integer push — likely eats return PC on 2nd MOV ***", flush=True)
        elif delta == -4:
            print("*** LONG integer push — return slot should be intact ***", flush=True)

    # step both MOVs + SETI to RTS
    for label in ("MOV R0", "MOV R1", "SETI"):
        print(f"=== step {label} ===", flush=True)
        buf.extend(mon(conn, "S", 0.6))
        data = mon(conn, "P", 1.2)
        buf.extend(data)
        pc, sp, ps, r0, r1 = parse_state(data)
        words = dump_around_sp(conn, buf, sp) if sp else []
        summarize(label, pc, sp, r0, r1, words)

    print("=== step RTS ===", flush=True)
    buf.extend(mon(conn, "S", 0.8))
    data = mon(conn, "P", 1.5)
    buf.extend(data)
    pc, sp, ps, r0, r1 = parse_state(data)
    summarize("AFTER RTS", pc, sp, r0, r1, [])
    if pc == "065054":
        print("*** BAD return PC=065054 ***", flush=True)
    elif pc and pc.startswith("03"):
        print(f"*** OK-looking return PC={pc} ***", flush=True)

    mon(conn, "B clear", 0.3)
    mon(conn, ">", 0.4)
    log.write_bytes(buf)
    print(f"log={log}", flush=True)
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
