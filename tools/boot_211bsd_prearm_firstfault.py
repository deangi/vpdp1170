#!/usr/bin/env python3
"""Arm B004332 BEFORE ':' CR so the first 065054 abort is caught with T active.

Uses runtime boot_script to answer ':' so we can stay in the monitor.
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

from benchmark_boot_times import TelnetConnection, BenchmarkError, enter_shell  # noqa: E402

HOST = "192.168.7.144"
COM = "COM18"
OUT = ROOT / "boot-benchmark-results-211bsd-postphys"
TRACE_N = 50000

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


def mon(conn, cmd: str, wait: float = 1.3) -> bytes:
    print(f"\n>>> {cmd}", flush=True)
    conn.send(cmd.encode() + b"\r")
    return drain(conn, wait)


def sh(conn, cmd: str, wait: float = 2.0) -> bytes:
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
    entries = [m for m in PC_RE.finditer(text)]
    lines = [f"trace entries: {len(entries)}"]

    def fmt(m):
        return (
            f"PC={m.group(1)} P={m.group(2)} PS={m.group(4)} ins={m.group(3)} "
            f"R0={m.group(5)} R1={m.group(6)} R2={m.group(7)} R3={m.group(8)} "
            f"R4={m.group(9)} R5={m.group(10)} SP={m.group(11)} {m.group(12).strip()}"
        )

    # Find last user-mode insn and any FP / transfer ops near end
    last_user = None
    for i, m in enumerate(entries):
        if (int(m.group(4), 8) & 0o140000) == 0o140000:
            last_user = i

    bad = None
    for i, m in enumerate(entries):
        if 0o060000 <= int(m.group(1), 8) < 0o100000:
            bad = i
            break

    lines.append(f"last_user={last_user} bad_page3={bad}")

    focus = bad if bad is not None else last_user
    if focus is None and entries:
        focus = len(entries) - 1

    if focus is not None:
        start = max(0, focus - 60)
        lines.append(f"window around index {focus}:")
        for j in range(start, min(len(entries), focus + 5)):
            mark = ">>>" if j == focus else "   "
            lines.append(f"{mark} {fmt(entries[j])}")

    # User-mode only list (compact)
    user_entries = [
        (i, m)
        for i, m in enumerate(entries)
        if (int(m.group(4), 8) & 0o140000) == 0o140000
    ]
    lines.append(f"user-mode traced: {len(user_entries)}")
    for i, m in user_entries[-40:]:
        lines.append(f"  [{i}] {fmt(m)}")

    # Opcode 17xxxx FP11
    fp = [(i, m) for i, m in enumerate(entries) if m.group(3).startswith("17")]
    lines.append(f"ins 17xxxx count: {len(fp)}")
    for i, m in fp[-40:]:
        lines.append(f"  [{i}] {fmt(m)}")

    # Transfers: JSR/JMP/RTS/RTT/SOB with interesting targets
    lines.append("transfer-ish near end (last 80 entries filtered):")
    for m in entries[-80:]:
        dis = m.group(12)
        if any(k in dis for k in ("JSR", "JMP", "RTS", "RTT", "SOB", "MARK", "SPL")):
            lines.append(f"  {fmt(m)}")

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
            if b"break pc=" in hit.lower() or b"stopped: monitor pause" in hit.lower():
                return bytes(hit)
        time.sleep(0.02)
    return bytes(hit)


def ensure_shell(conn) -> None:
    conn.send(b"\r")
    data = drain(conn, 1.0)
    if b"vpdp:" in data:
        return
    if b"monitor>" in data:
        mon(conn, ">", 0.5)
        return
    try:
        enter_shell(conn, 12.0, True)
    except Exception:
        conn.send(b"\x1b>>")
        drain(conn, 2.5)


def main() -> int:
    import serial

    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    com_log = OUT / f"{stamp}-prearm-firstfault-com.log"
    tel_log = OUT / f"{stamp}-prearm-firstfault-tel.log"
    analysis = OUT / f"{stamp}-prearm-firstfault-analysis.txt"
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
    time.sleep(0.08)
    ser.rts = False
    time.sleep(0.5)

    stop = threading.Event()
    threading.Thread(
        target=serial_reader, args=(ser, stop, com_log, shared), daemon=True
    ).start()

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

    ensure_shell(conn)

    # reset reloads pdpconfig.ini and wipes runtime boot_script, so:
    # reset #1 → set boot_script → reset #2 (applies script) → arm break.
    print("=== reset #1 (reload config) ===", flush=True)
    conn.send(b"reset\r")
    tel.extend(drain(conn, 4.0))
    ensure_shell(conn)

    print("=== set boot_script, reset #2 ===", flush=True)
    sh(conn, 'set boot_script=": => \\r"', 2.5)
    sh(conn, "set", 1.5)
    conn.send(b"reset\r")
    tel.extend(drain(conn, 5.0))

    # Arm breakpoint immediately while still in shell/monitor.
    ensure_shell(conn)
    sh(conn, "monitor", 1.5)
    mon(conn, "B clear", 0.3)
    mon(conn, f"T {TRACE_N}", 0.5)
    mon(conn, "B004332", 0.4)
    mon(conn, "C", 0.3)
    print("=== breakpoint armed; CPU running; waiting traps ===", flush=True)

    first_bad = None
    for attempt in range(80):
        print(f"\n=== trap {attempt+1} ===", flush=True)
        hit = wait_break(conn, 60.0)
        tel.extend(hit)
        if not hit:
            print("timeout waiting break", flush=True)
            mon(conn, "P", 1.5)
        data = mon(conn, "P", 1.2)
        udata = mon(conn, "U", 2.2)
        blob = (data + udata).decode("latin-1", errors="replace")
        mmr2 = None
        m = re.search(r"MMR2=([0-7]+)", blob)
        if m:
            mmr2 = m.group(1)
        pc_m = re.search(r"state: PC=([0-7]+)", blob)
        pc = pc_m.group(1) if pc_m else "?"
        print(f"trap{attempt+1}: PC={pc} MMR2={mmr2}", flush=True)

        # Guest console progress may appear on COM; peek shared
        if b"user mem" in shared.lower() or b"user mem" in tel.lower():
            print("[seen user mem on console]", flush=True)

        if mmr2 == "065054":
            first_bad = attempt + 1
            print("*** FIRST 065054 ***", flush=True)
            mon(conn, "MD147540:147630", 2.0)
            mon(conn, "D077122:077160", 1.5)
            # Dump mapped user text around likely sites
            mon(conn, "MI016000:016100", 1.8)
            mon(conn, "MI030000:031200", 2.5)
            mon(conn, "MI031060:031140", 1.8)
            mon(conn, "H", 0.5)
            time.sleep(2.0)
            break

        # If stuck at boot ':' console poll, inject CR via shell exit path.
        if pc == "012312" or (mmr2 == "012320"):
            print("*** stuck at ':' — inject CR via guest ***", flush=True)
            mon(conn, "C", 0.2)
            mon(conn, ">", 0.4)
            sh(conn, "exit", 1.5)
            try:
                conn.send(b"\r")
            except Exception as exc:
                print(f"CR failed: {exc}", flush=True)
            time.sleep(1.0)
            conn.send(b"\x1b>>")
            drain(conn, 2.0)
            ensure_shell(conn)
            sh(conn, "monitor", 1.5)
            mon(conn, f"T {TRACE_N}", 0.5)
            mon(conn, "B004332", 0.4)
            mon(conn, "C", 0.3)
            continue

        mon(conn, "C", 0.2)

    mon(conn, "B clear", 0.3)
    mon(conn, "T 0", 0.4)
    mon(conn, ">", 0.4)
    # Clear boot_script so later boots aren't surprised
    sh(conn, 'set boot_script=""', 2.0)

    tel_log.write_bytes(tel)
    stop.set()
    time.sleep(0.5)
    try:
        ser.close()
    except Exception:
        pass
    conn.close()

    print("\n=== ANALYSIS ===", flush=True)
    analyze(com_log, analysis)
    print(f"first_bad={first_bad}\ncom={com_log}\nanalysis={analysis}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
