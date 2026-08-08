#!/usr/bin/env python3
"""Catch SETI@031120 via COM BREAK (telnet does not print kek BREAK).

At that point both MOVs after STCFI F2,-(SP) have run. Distinguish:
  long (SETL): SP points at return; R0/R1 are the converted long
  short (SETI): SP points one word past stolen return; R1 holds former return
Then dump stack around SP and step RTS.
"""

from __future__ import annotations

import re
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from benchmark_boot_times import TelnetConnection, BenchmarkError  # noqa: E402

HOST = "192.168.7.144"
COM = "COM18"
OUT = ROOT / "boot-benchmark-results-211bsd-postphys"
# User-mode SETI after STCFI F2,-(SP). Kernel also uses VA 031120 early —
# ignore breaks unless PS current-mode is user (17xxxx).
BREAK_PC = "031120"


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
    m = re.search(
        r"state: PC=([0-7]+).*?\bR0=([0-7]+).*?\bR1=([0-7]+).*?\bR5=([0-7]+).*?\bSP=([0-7]+).*?\bPS=([0-7]+)",
        text,
        re.S,
    )
    if m:
        return {
            "pc": m.group(1),
            "r0": m.group(2),
            "r1": m.group(3),
            "r5": m.group(4),
            "sp": m.group(5),
            "ps": m.group(6),
        }
    return {}


def serial_reader(ser, stop, log_path, shared, hits):
    with log_path.open("ab", buffering=0) as fh:
        while not stop.is_set():
            try:
                n = ser.in_waiting
                chunk = ser.read(max(1, min(n, 4096))) if n else ser.read(1)
            except Exception as exc:
                print(f"\nCOM err: {exc}", flush=True)
                break
            if not chunk:
                continue
            fh.write(chunk)
            shared.extend(chunk)
            low = chunk.lower()
            if b"break pc=" in low or b"kek break" in shared[-200:].lower():
                hits.append(time.monotonic())


def main() -> int:
    import serial

    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    com_log = OUT / f"{stamp}-stcfi-com.log"
    tel_log = OUT / f"{stamp}-stcfi-tel.log"
    buf = bytearray()
    shared = bytearray()
    hits: list[float] = []

    print(f"=== open {COM} (no DTR/RTS toggle) ===", flush=True)
    ser = serial.Serial()
    ser.port = COM
    ser.baudrate = 115200
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    time.sleep(0.3)

    stop = threading.Event()
    thr = threading.Thread(
        target=serial_reader, args=(ser, stop, com_log, shared, hits), daemon=True
    )
    thr.start()

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
    if b"012312" in data or b"177560" in data:
        guest_cr(conn)

    mon(conn, "B clear", 0.4)
    mon(conn, f"B{BREAK_PC}", 0.5)
    mon(conn, "B", 0.6)
    mon(conn, "C", 0.3)

    print(f"=== wait USER-mode BREAK pc={BREAK_PC} (max 240s) ===", flush=True)
    deadline = time.monotonic() + 240.0
    st = {}
    user_hit = False
    hit_baseline = len(hits)
    while time.monotonic() < deadline:
        while len(hits) <= hit_baseline and time.monotonic() < deadline:
            try:
                chunk = conn.receive()
            except BenchmarkError:
                chunk = b""
            if chunk:
                buf.extend(chunk)
                sys.stdout.write(chunk.decode("latin-1", errors="replace"))
                sys.stdout.flush()
            time.sleep(0.05)
        if len(hits) <= hit_baseline:
            break
        print(f"*** COM BREAK #{len(hits) - hit_baseline} ***", flush=True)
        time.sleep(0.15)
        data = mon(conn, "P", 2.0)
        buf.extend(data)
        st = parse_state(data)
        ps = st.get("ps") or ""
        nxt = ""
        m_next = re.search(r"NEXT=\d+:([0-7]+)", data.decode("latin-1", errors="replace"))
        if m_next:
            nxt = m_next.group(1)
        print(
            f"    PC={st.get('pc')} PS={ps} NEXT_ins={nxt} "
            f"SP={st.get('sp')} R0={st.get('r0')} R1={st.get('r1')}",
            flush=True,
        )
        # User mode: PSW bits 15-14 == 11
        ps_val = int(ps, 8) if ps else 0
        is_user = (ps_val & 0o140000) == 0o140000
        is_seti = nxt == "170002"
        if is_user and is_seti:
            user_hit = True
            break
        # Wrong context (kernel 031120 etc.) — continue; C sets skip_once
        print("    (not user SETI — continue)", flush=True)
        hit_baseline = len(hits)
        mon(conn, "C", 0.3)

    if not user_hit:
        data = mon(conn, "P", 2.0)
        buf.extend(data)
        st = parse_state(data)
        print(
            f"TIMEOUT; paused at PC={st.get('pc')} PS={st.get('ps')} SP={st.get('sp')}",
            flush=True,
        )
        mon(conn, "B clear", 0.3)
        mon(conn, ">", 0.4)
        tel_log.write_bytes(buf)
        print(f"tel={tel_log}\ncom={com_log}", flush=True)
        stop.set()
        thr.join(timeout=2)
        ser.close()
        conn.close()
        return 1

    print(
        f"*** USER SETI AT {st.get('pc')} SP={st.get('sp')} R0={st.get('r0')} "
        f"R1={st.get('r1')} R5={st.get('r5')} PS={st.get('ps')}",
        flush=True,
    )

    sp = st.get("sp")
    if sp:
        spo = int(sp, 8)
        # M uses I-space peek — useless for user stack. Use physical via D-PAR.
        # User D page 7 PAR was 013716 in prior runs; re-read from U above.
        buf.extend(mon(conn, "U", 3.0))
        u_text = buf.decode("latin-1", errors="replace")
        # Prefer last User D-PAR page 7 line: "  7  ...  <dpar> <dpdr> <dphys>"
        dpar = None
        for line in u_text.splitlines():
            m = re.match(
                r"^\s*7\s+[0-7]+\s+[0-7]+\s+[0-7]+\s+([0-7]+)\s+[0-7]+\s+([0-7]+)",
                line,
            )
            if m:
                dpar = int(m.group(1), 8)
        if dpar is not None:
            off = spo & 0o17777
            phys = (dpar << 6) + off
            print(
                f"*** phys SP: D-PAR={dpar:06o} off={off:06o} phys={phys:08o}",
                flush=True,
            )
            lo = phys - 0o10
            hi = phys + 0o20
            d = mon(conn, f"D{lo:08o}:{hi:08o}", 2.5)
            buf.extend(d)
            print("*** physical stack ***", flush=True)
            print(d.decode("latin-1", errors="replace"), flush=True)
            m = re.search(rf"{phys:08o}:\s*([0-7]+)", d.decode("latin-1", errors="replace"))
            # also match 6-digit if tool prints without leading 0
            if not m:
                m = re.search(rf"{phys:06o}:\s*([0-7]+)", d.decode("latin-1", errors="replace"))
            ret = m.group(1) if m else "?"
            print(f"*** word@SP phys (RTS return) = {ret}", flush=True)
            if ret == "065054":
                print("*** return already 065054 at SETI — STCFI epilogue did not create it ***", flush=True)
            r1 = st.get("r1")
            print(
                f"*** R0:R1={st.get('r0')}:{r1} (STCFI long result if SETL)",
                flush=True,
            )
        # Code bytes (I-space M works)
        buf.extend(mon(conn, "MI031040:031130", 2.5))

    buf.extend(mon(conn, "H", 0.5))
    time.sleep(1.0)

    # Step SETI (already at SETI — step to RTS) then RTS
    print("=== step to RTS ===", flush=True)
    if st.get("pc") == "031120":
        buf.extend(mon(conn, "S", 0.6))
        data = mon(conn, "P", 1.2)
        buf.extend(data)
        st = parse_state(data)
        print(f"after SETI step: PC={st.get('pc')} SP={st.get('sp')}", flush=True)

    print("=== step RTS ===", flush=True)
    buf.extend(mon(conn, "S", 0.8))
    data = mon(conn, "P", 1.5)
    buf.extend(data)
    st = parse_state(data)
    print(
        f"*** AFTER RTS PC={st.get('pc')} SP={st.get('sp')} R0={st.get('r0')} R1={st.get('r1')}",
        flush=True,
    )
    if st.get("pc") == "065054":
        print("*** CONFIRMED bad return ***", flush=True)

    mon(conn, "B clear", 0.3)
    mon(conn, ">", 0.4)
    tel_log.write_bytes(buf)
    print(f"tel={tel_log}\ncom={com_log}", flush=True)
    stop.set()
    thr.join(timeout=2)
    ser.close()
    conn.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
