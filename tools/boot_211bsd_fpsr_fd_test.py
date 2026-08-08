#!/usr/bin/env python3
"""After FPSR-capable flash: break user SETL@031064, show FPSR, clear FD, continue.

Verifies the MODF-(PC)+ skip theory:
  - FPSR bit 0200 (FD) should be set at SETL
  - Clearing FD (keep SETL/FL=0100) should let SUBF/STCFI run and RTS to 016036
  - Then boot may progress past user mem
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
BREAK_PC = "031064"
SETL_INS = "170012"
CLEAR_FD_KEEP_FL = "000100"  # SETL only; FD cleared


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
        r"state: PC=([0-7]+).*?\bSP=([0-7]+).*?\bPS=([0-7]+)"
        r"(?:.*?\bFPSR=([0-7]+))?",
        text,
        re.S,
    )
    out = {}
    if m:
        out["pc"], out["sp"], out["ps"] = m.group(1), m.group(2), m.group(3)
        if m.group(4):
            out["fpsr"] = m.group(4)
    m2 = re.search(r"\bFPSR=([0-7]+)", text)
    if m2:
        out["fpsr"] = m2.group(1)
    m3 = re.search(r"\bR0=([0-7]+).*?\bR1=([0-7]+).*?\bR5=([0-7]+)", text, re.S)
    if m3:
        out["r0"], out["r1"], out["r5"] = m3.group(1), m3.group(2), m3.group(3)
    m4 = re.search(r"NEXT=[0-7]+:([0-7]+)", text)
    if m4:
        out["next"] = m4.group(1)
    return out


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
            if b"break pc=" in chunk.lower() or b"kek break" in shared[-240:].lower():
                hits.append(time.monotonic())


def main() -> int:
    import serial

    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    com_log = OUT / f"{stamp}-fpsr-fd-com.log"
    tel_log = OUT / f"{stamp}-fpsr-fd-tel.log"
    buf = bytearray()
    shared = bytearray()
    hits: list[float] = []

    print(f"=== open {COM} ===", flush=True)
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
    for i in range(15):
        try:
            conn.connect()
            break
        except Exception as exc:
            print("telnet", i, exc, flush=True)
            time.sleep(2)
            conn = TelnetConnection(HOST, 23, timeout=2.0)

    to_shell(conn)
    to_monitor(conn)
    help_txt = mon(conn, "?", 1.5)
    buf.extend(help_txt)
    if b"FPSR" not in help_txt:
        print("*** WARN: FPSR not in help — flash may still be old ***", flush=True)

    mon(conn, ">", 0.4)
    print("=== reset ===", flush=True)
    send_line(conn, "reset")
    buf.extend(drain(conn, 8.0))
    for _ in range(12):
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
    mon(conn, "C", 0.3)

    print(f"=== wait USER SETL @{BREAK_PC} (max 240s) ===", flush=True)
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
        time.sleep(0.15)
        data = mon(conn, "P", 2.0)
        buf.extend(data)
        st = parse_state(data)
        ps_val = int(st.get("ps") or "0", 8)
        is_user = (ps_val & 0o140000) == 0o140000
        is_setl = st.get("next") == SETL_INS
        print(
            f"    PC={st.get('pc')} PS={st.get('ps')} NEXT={st.get('next')} "
            f"FPSR={st.get('fpsr')} SP={st.get('sp')}",
            flush=True,
        )
        if is_user and is_setl and st.get("pc") == BREAK_PC:
            user_hit = True
            break
        print("    (skip — continue)", flush=True)
        hit_baseline = len(hits)
        mon(conn, "C", 0.3)

    if not user_hit:
        print("TIMEOUT", flush=True)
        mon(conn, "B clear", 0.3)
        mon(conn, ">", 0.4)
        tel_log.write_bytes(buf)
        print(f"tel={tel_log}\ncom={com_log}", flush=True)
        stop.set()
        thr.join(timeout=2)
        ser.close()
        conn.close()
        return 1

    fpsr = st.get("fpsr")
    print(f"*** USER SETL FPSR={fpsr} SP={st.get('sp')} R0={st.get('r0')} R1={st.get('r1')}", flush=True)
    if fpsr is None:
        print("*** ERROR: FPSR missing from state — old firmware? ***", flush=True)
    else:
        fpsr_v = int(fpsr, 8)
        fd = bool(fpsr_v & 0o200)
        fl = bool(fpsr_v & 0o100)
        print(f"*** FD(double)={fd} FL(long)={fl} ***", flush=True)
        if fd:
            print("*** CONFIRMED FD set — clearing FD, keeping FL ***", flush=True)
            buf.extend(mon(conn, f"FPSR={CLEAR_FD_KEEP_FL}", 0.8))
            data = mon(conn, "P", 1.5)
            buf.extend(data)
            st = parse_state(data)
            print(f"*** after patch FPSR={st.get('fpsr')} ***", flush=True)
        else:
            print("*** FD clear unexpectedly — not the double-imm theory ***", flush=True)

    spo = int(st.get("sp") or "0", 8)
    buf.extend(mon(conn, f"MD{spo:06o}:{spo + 0o10:06o}", 2.0))

    # Single-step MODF region: SETL .. through RTS
    print("=== step to RTS (expect SUBF/STCFI if FD clear) ===", flush=True)
    saw_stcfi = False
    saw_subf = False
    for _ in range(25):
        buf.extend(mon(conn, "S", 0.45))
        data = mon(conn, "P", 0.9)
        buf.extend(data)
        st = parse_state(data)
        text = data.decode("latin-1", errors="replace")
        pc = st.get("pc")
        if "175646" in text or "STCFI" in text:
            saw_stcfi = True
        if "173201" in text or "SUBF" in text:
            saw_subf = True
        print(
            f"    PC={pc} SP={st.get('sp')} FPSR={st.get('fpsr')} "
            f"R0={st.get('r0')} R1={st.get('r1')}",
            flush=True,
        )
        if pc == "031122":
            break
        if pc == "065054":
            print("*** hit bad PC early ***", flush=True)
            break
        if pc and not str(pc).startswith("031"):
            break

    print(f"*** saw SUBF={saw_subf} STCFI={saw_stcfi} ***", flush=True)
    if st.get("pc") == "031122":
        md = mon(conn, f"MD{int(st['sp'], 8):06o}:{int(st['sp'], 8) + 2:06o}", 1.5)
        buf.extend(md)
        m = re.search(rf"{st['sp']}:\s*([0-7]+)", md.decode("latin-1", errors="replace"))
        ret = m.group(1) if m else "?"
        print(f"*** pre-RTS return = {ret} (want 016036) ***", flush=True)
        buf.extend(mon(conn, "S", 0.6))
        data = mon(conn, "P", 1.2)
        buf.extend(data)
        st = parse_state(data)
        print(f"*** AFTER RTS PC={st.get('pc')} (want 016036, not 065054) ***", flush=True)

    # Continue boot and watch for progress / hang
    mon(conn, "B clear", 0.3)
    print("=== continue boot 90s ===", flush=True)
    mon(conn, "C", 0.3)
    mon(conn, ">", 0.4)
    send_line(conn, "exit")
    drain(conn, 1.0)
    end = time.monotonic() + 90.0
    guest = bytearray()
    while time.monotonic() < end:
        try:
            chunk = conn.receive()
        except BenchmarkError:
            chunk = b""
        if chunk:
            guest.extend(chunk)
            buf.extend(chunk)
            sys.stdout.write(chunk.decode("latin-1", errors="replace"))
            sys.stdout.flush()
            low = guest.lower()
            if b"login:" in low or b"#" in guest[-20:] or b"erase" in low:
                print("\n*** BOOT PROGRESS marker seen ***", flush=True)
                break
        time.sleep(0.05)

    conn.send(b"\x1b>>")
    drain(conn, 3.0)
    to_monitor(conn)
    data = mon(conn, "P", 2.0)
    buf.extend(data)
    st = parse_state(data)
    print(f"*** final PC={st.get('pc')} PS={st.get('ps')} SP={st.get('sp')} ***", flush=True)

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
