#!/usr/bin/env python3
"""Catch FIRST MMR2=065054 abort: pause at 'phys mem', T+B004332, analyze."""

from __future__ import annotations

import re
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from benchmark_boot_times import TelnetConnection, BenchmarkError, enter_shell  # noqa: E402

HOST = "192.168.7.144"
COM = "COM18"
OUT = ROOT / "boot-benchmark-results-211bsd-postphys"
TRACE_N = 30000

PC_RE = re.compile(
    r"kek trace: PC=([0-7]+)\s+P=([0-7]+)\s+ins=([0-7]+)\s+PS=([0-7]+)\s+"
    r"R0=([0-7]+)\s+R1=([0-7]+)\s+R2=([0-7]+)\s+R3=([0-7]+)\s+"
    r"R4=([0-7]+)\s+R5=([0-7]+)\s+SP=([0-7]+)\s+(.*)$",
    re.MULTILINE,
)


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


def mon(conn, cmd: str, wait: float = 1.4) -> bytes:
    print(f"\n>>> {cmd}", flush=True)
    conn.send(cmd.encode() + b"\r")
    return drain(conn, wait)


def serial_reader(ser, stop, log_path, shared):
    with log_path.open("wb") as f:
        while not stop.is_set():
            try:
                data = ser.read(8192)
            except Exception as exc:
                print(f"\nCOM err: {exc}", flush=True)
                break
            if data:
                shared.extend(data)
                f.write(data)
                f.flush()


def analyze(com_path: Path, out_path: Path) -> None:
    text = com_path.read_text("latin-1", errors="replace")
    entries = []
    for m in PC_RE.finditer(text):
        entries.append(
            {
                "pc": m.group(1),
                "phys": m.group(2),
                "ins": m.group(3),
                "ps": m.group(4),
                "r0": m.group(5),
                "r1": m.group(6),
                "r2": m.group(7),
                "r3": m.group(8),
                "r4": m.group(9),
                "r5": m.group(10),
                "sp": m.group(11),
                "dis": m.group(12).strip(),
            }
        )

    def fmt(e):
        return (
            f"PC={e['pc']} P={e['phys']} PS={e['ps']} ins={e['ins']} "
            f"R0={e['r0']} R1={e['r1']} R2={e['r2']} R3={e['r3']} "
            f"R4={e['r4']} R5={e['r5']} SP={e['sp']} {e['dis']}"
        )

    lines = [f"trace entries: {len(entries)}"]

    # Last user-mode (PS current mode == 3 => bits 14:13 = 11 => 140000)
    last_user_idx = None
    for i, e in enumerate(entries):
        try:
            ps = int(e["ps"], 8)
        except ValueError:
            continue
        if (ps & 0o140000) == 0o140000:
            last_user_idx = i

    # First PC in 060000-077777 (any mode)
    bad_idx = None
    for i, e in enumerate(entries):
        try:
            pc = int(e["pc"], 8)
        except ValueError:
            continue
        if 0o060000 <= pc < 0o100000:
            bad_idx = i
            break

    # Find RTT that restores toward bad PC: look for RTT with SP becoming user stack
    rtt_idxs = [i for i, e in enumerate(entries) if "RTT" in e["dis"]]

    lines.append(f"last_user_idx={last_user_idx} bad_idx={bad_idx} rtt_count={len(rtt_idxs)}")

    if last_user_idx is not None:
        start = max(0, last_user_idx - 40)
        lines.append("window around last user-mode traced insn:")
        for j in range(start, min(len(entries), last_user_idx + 8)):
            mark = ">>>" if j == last_user_idx else "   "
            lines.append(f"{mark} {fmt(entries[j])}")

    if bad_idx is not None:
        start = max(0, bad_idx - 40)
        lines.append("window around first page3 PC:")
        for j in range(start, min(len(entries), bad_idx + 5)):
            mark = ">>>" if j == bad_idx else (" ->" if j == bad_idx - 1 else "   ")
            lines.append(f"{mark} {fmt(entries[j])}")
    else:
        lines.append("no page3 PC in trace (fetch abort may omit it)")
        # Show last 50 before end / around first RTT with PS=170000
        for i, e in enumerate(entries):
            if "RTT" in e["dis"] and e["ps"].startswith("17"):
                start = max(0, i - 50)
                lines.append(f"window before first user-PS RTT at {i}:")
                for j in range(start, min(len(entries), i + 3)):
                    mark = ">>>" if j == i else "   "
                    lines.append(f"{mark} {fmt(entries[j])}")
                break
        else:
            lines.append("last 50:")
            for e in entries[-50:]:
                lines.append(f"  {fmt(e)}")

    # FP ops in trace
    fp_hits = [
        (i, e)
        for i, e in enumerate(entries)
        if any(
            k in e["dis"]
            for k in (
                "MODF",
                "MODD",
                "MULF",
                "MULD",
                "ADDF",
                "SUBF",
                "DIVF",
                "LDF",
                "STF",
                "CLRF",
                "ABS",
                "NEG",
                "LDEXP",
                "STEXP",
                "LDCIF",
                "STCFI",
                "LDCDF",
                "STCFD",
            )
        )
        or e["ins"].startswith("17")
    ]
    lines.append(f"fp-ish entries: {len(fp_hits)}")
    for i, e in fp_hits[-30:]:
        lines.append(f"  [{i}] {fmt(e)}")

    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines), flush=True)


def wait_break(conn, seconds: float) -> bytes:
    deadline = time.monotonic() + seconds
    hit = bytearray()
    while time.monotonic() < deadline:
        try:
            chunk = conn.receive()
        except BenchmarkError:
            chunk = b""
        if chunk:
            hit.extend(chunk)
            sys.stdout.write(chunk.decode("latin-1", errors="replace"))
            sys.stdout.flush()
            low = hit.lower()
            if b"break pc=" in low or b"stopped: monitor pause pc=" in low:
                return bytes(hit)
        time.sleep(0.02)
    return bytes(hit)


def main() -> int:
    import serial

    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    com_log = OUT / f"{stamp}-physmem-firstfault-com.log"
    tel_log = OUT / f"{stamp}-physmem-firstfault-tel.log"
    analysis = OUT / f"{stamp}-physmem-firstfault-analysis.txt"
    shared = bytearray()
    tel = bytearray()

    ser = serial.Serial()
    ser.port = COM
    ser.baudrate = 115200
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    time.sleep(0.2)
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False
    time.sleep(0.8)

    stop = threading.Event()
    thr = threading.Thread(
        target=serial_reader, args=(ser, stop, com_log, shared), daemon=True
    )
    thr.start()

    conn = None
    for attempt in range(20):
        try:
            conn = TelnetConnection(HOST, 23, timeout=2.0)
            conn.connect()
            break
        except Exception as exc:
            print(f"telnet {attempt}: {exc}", flush=True)
            time.sleep(2.0)
    if not conn:
        raise RuntimeError("telnet")

    conn.send(b"\r")
    data = drain(conn, 1.0)
    tel.extend(data)
    if b"monitor>" in data:
        mon(conn, ">", 0.4)
    elif b"vpdp:" not in data:
        try:
            enter_shell(conn, 12.0, True)
        except Exception:
            conn.send(b"\x1b>>")
            drain(conn, 2.5)

    print("=== reset ===", flush=True)
    conn.send(b"reset\r")
    try:
        tel.extend(drain(conn, 4.0))
        conn.send(b"exit\r")
        tel.extend(drain(conn, 2.0))
    except Exception as exc:
        print(f"reconnect after reset: {exc}", flush=True)
        time.sleep(3.0)
        conn = TelnetConnection(HOST, 23, timeout=2.0)
        for _ in range(15):
            try:
                conn.connect()
                break
            except Exception:
                time.sleep(2.0)
        conn.send(b"\r")
        drain(conn, 1.0)

    print("=== ':' / CR ===", flush=True)
    deadline = time.monotonic() + 90.0
    while time.monotonic() < deadline:
        try:
            chunk = conn.receive()
        except BenchmarkError:
            chunk = b""
        if chunk:
            tel.extend(chunk)
            sys.stdout.write(chunk.decode("latin-1", errors="replace"))
            sys.stdout.flush()
        window = (tel[-300:] + shared[-300:]).replace(b"\r", b"\n")
        if any(line.strip() == ":" for line in window.decode("latin-1", "replace").splitlines()[-12:]):
            break
        time.sleep(0.05)
    conn.send(b"\r")

    print("=== wait 'phys mem' (early pause) ===", flush=True)
    deadline = time.monotonic() + 90.0
    while time.monotonic() < deadline:
        try:
            chunk = conn.receive()
        except BenchmarkError:
            chunk = b""
        if chunk:
            tel.extend(chunk)
            sys.stdout.write(chunk.decode("latin-1", errors="replace"))
            sys.stdout.flush()
            if b"phys mem" in tel.lower():
                break
        time.sleep(0.02)
    tel.extend(drain(conn, 0.15))

    print("\n=== pause at phys mem ===", flush=True)
    conn.send(b"\x1b>>")
    drain(conn, 2.0)
    mon(conn, "monitor", 1.5)
    mon(conn, "P", 2.0)
    mon(conn, "U", 3.0)

    print(f"\n=== arm T {TRACE_N} B004332 ===", flush=True)
    mon(conn, "B clear", 0.3)
    mon(conn, f"T {TRACE_N}", 0.5)
    mon(conn, "B004332", 0.4)

    first_bad = None
    for attempt in range(40):
        print(f"\n=== trap attempt {attempt+1} ===", flush=True)
        mon(conn, "C", 0.2)
        hit = wait_break(conn, 20.0)
        tel.extend(hit)
        data = mon(conn, "P", 1.2)
        udata = mon(conn, "U", 2.5)
        text = (data + udata).decode("latin-1", errors="replace")
        mmr2 = None
        for line in text.splitlines():
            if "MMR2=" in line:
                m = re.search(r"MMR2=([0-7]+)", line)
                if m:
                    mmr2 = m.group(1)
        pc = None
        m = re.search(r"state: PC=([0-7]+)", text)
        if m:
            pc = m.group(1)
        print(f"trap{attempt+1}: PC={pc} MMR2={mmr2}", flush=True)
        if mmr2 == "065054" or (pc == "004332" and "MMR2=065054" in text):
            first_bad = attempt + 1
            print("*** FIRST 065054 ABORT ***", flush=True)
            mon(conn, "MD147540:147630", 2.0)
            mon(conn, "D077122:077160", 1.5)
            mon(conn, "H", 0.5)
            time.sleep(1.5)
            break
        # If still booting (no abort mmr), keep going
        if attempt >= 35:
            print("too many traps without 065054", flush=True)
            break

    mon(conn, "B clear", 0.3)
    mon(conn, "T 0", 0.4)

    tel_log.write_bytes(tel)
    stop.set()
    time.sleep(0.6)
    try:
        ser.close()
    except Exception:
        pass
    conn.close()

    print("\n=== ANALYSIS ===", flush=True)
    analyze(com_log, analysis)
    print(f"first_bad_attempt={first_bad}\ncom={com_log}\nanalysis={analysis}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
