#!/usr/bin/env python3
"""Break at user SETL @031064; dump MD stack/frame; identify planted return PC.

Reflash must include MI/MD/MP monitor commands.
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
BREAK_PC = "031064"  # SETL — start of long-int FP epilogue block
SETL_INS = "170012"


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
        r"state: PC=([0-7]+).*?\bR0=([0-7]+).*?\bR1=([0-7]+).*?\bR2=([0-7]+)"
        r".*?\bR3=([0-7]+).*?\bR4=([0-7]+).*?\bR5=([0-7]+).*?\bSP=([0-7]+)"
        r".*?\bPS=([0-7]+)",
        text,
        re.S,
    )
    if not m:
        return {}
    return {
        "pc": m.group(1),
        "r0": m.group(2),
        "r1": m.group(3),
        "r2": m.group(4),
        "r3": m.group(5),
        "r4": m.group(6),
        "r5": m.group(7),
        "sp": m.group(8),
        "ps": m.group(9),
    }


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


def dump_words(label: str, text: str) -> dict[str, str]:
    """Parse MD/MI/D dump lines into addr->word."""
    out: dict[str, str] = {}
    for line in text.splitlines():
        m = re.match(r"^([0-7]+):\s+((?:[0-7]+(?:\s+|$))+)", line.strip())
        if not m:
            continue
        base = int(m.group(1), 8)
        for i, v in enumerate(m.group(2).split()):
            out[f"{base + i * 2:06o}"] = v
    print(f"*** {label}: {len(out)} words ***", flush=True)
    return out


def main() -> int:
    import serial

    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    com_log = OUT / f"{stamp}-setl-com.log"
    tel_log = OUT / f"{stamp}-setl-tel.log"
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
    try:
        ser.open()
    except Exception as exc:
        print(f"COM open failed: {exc}", flush=True)
        return 1
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
    else:
        print("telnet failed", flush=True)
        stop.set()
        ser.close()
        return 1

    to_shell(conn)
    # Smoke-test new commands exist
    to_monitor(conn)
    help_txt = mon(conn, "?", 1.5)
    buf.extend(help_txt)
    if b"MI00100" not in help_txt and b"MD00100" not in help_txt:
        print("*** WARN: MI/MD not in help — firmware may predate reflash ***", flush=True)

    print("=== reset ===", flush=True)
    mon(conn, ">", 0.4)
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
    # Arm a short live trace so H has lead-in if ring allocates with T
    mon(conn, "T 20000", 0.6)
    mon(conn, "C", 0.3)

    print(f"=== wait USER SETL pc={BREAK_PC} (max 240s) ===", flush=True)
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
        text = data.decode("latin-1", errors="replace")
        nxt = ""
        m_next = re.search(r"NEXT=[0-7]+:([0-7]+)", text)
        if m_next:
            nxt = m_next.group(1)
        ps = st.get("ps") or ""
        ps_val = int(ps, 8) if ps else 0
        is_user = (ps_val & 0o140000) == 0o140000
        is_setl = nxt == SETL_INS
        print(
            f"    PC={st.get('pc')} PS={ps} NEXT={nxt} "
            f"SP={st.get('sp')} R5={st.get('r5')} user={is_user} setl={is_setl}",
            flush=True,
        )
        if is_user and is_setl and st.get("pc") == BREAK_PC:
            user_hit = True
            break
        print("    (not user SETL — continue)", flush=True)
        hit_baseline = len(hits)
        mon(conn, "C", 0.3)

    if not user_hit:
        data = mon(conn, "P", 2.0)
        buf.extend(data)
        st = parse_state(data)
        print(
            f"TIMEOUT; PC={st.get('pc')} PS={st.get('ps')} SP={st.get('sp')}",
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
        f"*** USER SETL AT PC={st.get('pc')} SP={st.get('sp')} R5={st.get('r5')} "
        f"R0={st.get('r0')} R1={st.get('r1')} R2={st.get('r2')} "
        f"R3={st.get('r3')} R4={st.get('r4')} PS={st.get('ps')}",
        flush=True,
    )

    sp = st.get("sp")
    r5 = st.get("r5")
    spo = int(sp, 8) if sp else 0
    r5o = int(r5, 8) if r5 else spo

    buf.extend(mon(conn, "U", 3.0))

    # D-space stack: from a bit below SP up through R5 frame
    lo = max(0, min(spo, r5o) - 0o20)
    hi = max(spo, r5o) + 0o40
    md = mon(conn, f"MD{lo:06o}:{hi:06o}", 3.0)
    buf.extend(md)
    words = dump_words("MD stack/frame", md.decode("latin-1", errors="replace"))
    print(md.decode("latin-1", errors="replace"), flush=True)

    ret = words.get(sp, "?")
    print(f"*** word@SP (RTS return slot) = {ret}", flush=True)
    if ret == "065054":
        print("*** BAD return already present at SETL — planted before this block ***", flush=True)
    elif ret.startswith("03") or ret.startswith("00"):
        print(f"*** return looks like code PC={ret} ***", flush=True)

    # Words around SP annotated
    print("*** SP neighborhood ***", flush=True)
    for off in range(-8, 16, 2):
        a = f"{(spo + off) & 0o177777:06o}"
        mark = ""
        if off == 0:
            mark = " <--SP return"
        elif r5 and a == r5:
            mark = " <--R5"
        print(f"    {a}: {words.get(a, '------')}{mark}", flush=True)

    if r5:
        print("*** R5 frame neighborhood ***", flush=True)
        for off in range(-4, 20, 2):
            a = f"{(r5o + off) & 0o177777:06o}"
            mark = " <--R5" if off == 0 else ""
            print(f"    {a}: {words.get(a, '------')}{mark}", flush=True)

    # I-space function body
    mi = mon(conn, "MI031040:031130", 2.5)
    buf.extend(mi)
    print("*** MI function ***", flush=True)
    print(mi.decode("latin-1", errors="replace"), flush=True)

    # Physical via MP as cross-check
    u_text = buf.decode("latin-1", errors="replace")
    dpar = None
    in_user = False
    for line in u_text.splitlines():
        if "User PAR/PDR:" in line:
            in_user = True
            continue
        if in_user and line.strip().startswith("Unibus"):
            break
        if not in_user:
            continue
        m = re.match(
            r"^\s*7\s+[0-7]+\s+[0-7]+\s+[0-7]+\s+([0-7]+)\s+[0-7]+\s+([0-7]+)",
            line,
        )
        if m:
            dpar = int(m.group(1), 8)
    if dpar is not None and sp:
        off = spo & 0o17777
        phys = (dpar << 6) + off
        print(f"*** MP cross-check phys={phys:08o} ***", flush=True)
        mp = mon(conn, f"MP{phys - 0o10:08o}:{phys + 0o20:08o}", 2.5)
        buf.extend(mp)
        print(mp.decode("latin-1", errors="replace"), flush=True)

    buf.extend(mon(conn, "H", 0.5))
    time.sleep(1.5)

    # Step through to RTS and confirm
    print("=== step through epilogue to RTS ===", flush=True)
    for _ in range(20):
        buf.extend(mon(conn, "S", 0.5))
        data = mon(conn, "P", 1.0)
        buf.extend(data)
        st = parse_state(data)
        pc = st.get("pc")
        print(f"    step PC={pc} SP={st.get('sp')} R0={st.get('r0')} R1={st.get('r1')}", flush=True)
        if pc == "031122":
            break
        if pc and not pc.startswith("031"):
            break

    if st.get("pc") == "031122":
        md2 = mon(conn, f"MD{int(st['sp'], 8):06o}:{int(st['sp'], 8) + 2:06o}", 1.5)
        buf.extend(md2)
        w2 = dump_words("pre-RTS", md2.decode("latin-1", errors="replace"))
        print(f"*** pre-RTS return = {w2.get(st['sp'], '?')}", flush=True)
        buf.extend(mon(conn, "S", 0.6))
        data = mon(conn, "P", 1.2)
        buf.extend(data)
        st = parse_state(data)
        print(f"*** AFTER RTS PC={st.get('pc')} SP={st.get('sp')}", flush=True)

    mon(conn, "B clear", 0.3)
    mon(conn, "T 0", 0.3)
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
