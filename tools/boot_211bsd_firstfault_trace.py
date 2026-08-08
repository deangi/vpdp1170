#!/usr/bin/env python3
"""Find what first loads bad PC=065054 after 2.11BSD user mem.

Flow:
  1. Open COM18 for kek trace capture
  2. Management-shell reset + exit; CR at ':'
  3. On 'user mem', enter monitor, pause ASAP
  4. Arm T <N> + B004332, continue
  5. On first trap: dump U/regs/stack; keep COM trace
  6. Parse COM for last user PCs before 065054 / abort
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

from benchmark_boot_times import (  # noqa: E402
    TelnetConnection,
    BenchmarkError,
    enter_shell,
)

HOST = "192.168.7.144"
COM = "COM18"
OUT = ROOT / "boot-benchmark-results-211bsd-postphys"
TRACE_N = 8000

PC_RE = re.compile(
    r"kek trace: PC=([0-7]+)\s+P=[0-7]+\s+ins=([0-7]+)\s+PS=([0-7]+)\s+"
    r"R0=([0-7]+)\s+R1=([0-7]+)\s+R2=([0-7]+)\s+R3=([0-7]+)\s+"
    r"R4=([0-7]+)\s+R5=([0-7]+)\s+SP=([0-7]+)\s+(.*)$",
    re.MULTILINE,
)
STATE_PC_RE = re.compile(r"state: PC=([0-7]+)")


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


def mon(conn, cmd: str, wait: float = 1.5) -> bytes:
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


def analyze_trace(com_bytes: bytes, out_path: Path) -> None:
    text = com_bytes.decode("latin-1", errors="replace")
    entries = []
    for m in PC_RE.finditer(text):
        entries.append(
            {
                "pc": m.group(1),
                "ins": m.group(2),
                "ps": m.group(3),
                "r0": m.group(4),
                "r1": m.group(5),
                "r2": m.group(6),
                "r3": m.group(7),
                "r4": m.group(8),
                "r5": m.group(9),
                "sp": m.group(10),
                "dis": m.group(11).strip(),
            }
        )

    def fmt(e):
        return (
            f"PC={e['pc']} PS={e['ps']} ins={e['ins']} "
            f"R0={e['r0']} R1={e['r1']} SP={e['sp']} {e['dis']}"
        )

    lines = [f"trace entries: {len(entries)}"]
    bad_idx = None
    for i, e in enumerate(entries):
        try:
            pci = int(e["pc"], 8)
        except ValueError:
            continue
        if 0o060000 <= pci < 0o100000:
            bad_idx = i
            break

    if bad_idx is None:
        lines.append("no PC in 060000-077777 found in trace")
        lines.append("last 40 entries:")
        for e in entries[-40:]:
            lines.append(f"  {fmt(e)}")
    else:
        start = max(0, bad_idx - 80)
        lines.append(
            f"first PC in page3-range at index {bad_idx}: PC={entries[bad_idx]['pc']}"
        )
        lines.append("prior instruction (control transfer candidate):")
        if bad_idx > 0:
            lines.append(f"  {fmt(entries[bad_idx - 1])}")
        lines.append(f"window [{start} .. {min(len(entries), bad_idx + 5)}):")
        for j in range(start, min(len(entries), bad_idx + 6)):
            mark = ">>>" if j == bad_idx else (" ->" if j == bad_idx - 1 else "   ")
            lines.append(f"{mark} {fmt(entries[j])}")

    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines), flush=True)
    print(f"analysis={out_path}", flush=True)


def main() -> int:
    import serial

    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    com_log = OUT / f"{stamp}-firstfault-com.log"
    tel_log = OUT / f"{stamp}-firstfault-tel.log"
    analysis = OUT / f"{stamp}-firstfault-analysis.txt"
    shared = bytearray()
    tel = bytearray()

    print(f"=== open {COM} ===", flush=True)
    ser = serial.Serial()
    ser.port = COM
    ser.baudrate = 115200
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    time.sleep(0.3)
    # Gentle EN pulse after open (board may be half-odd)
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False
    ser.dtr = False
    time.sleep(1.0)

    stop = threading.Event()
    thr = threading.Thread(
        target=serial_reader, args=(ser, stop, com_log, shared), daemon=True
    )
    thr.start()

    print("=== telnet ===", flush=True)
    conn = None
    for attempt in range(25):
        try:
            conn = TelnetConnection(HOST, 23, timeout=2.0)
            conn.connect()
            print(f"connected {attempt}", flush=True)
            break
        except Exception as exc:
            print(f"wait {attempt}: {exc}", flush=True)
            time.sleep(2.0)
    if conn is None:
        raise RuntimeError("telnet down")

    conn.send(b"\r")
    data = drain(conn, 1.0)
    tel.extend(data)
    if b"monitor>" in data:
        mon(conn, ">", 0.5)
    elif b"vpdp:" not in data:
        try:
            enter_shell(conn, 12.0, True)
        except Exception:
            conn.send(b"\x1b>>")
            drain(conn, 2.5)

    conn.send(b"\r")
    drain(conn, 0.4)

    print("=== reset ===", flush=True)
    conn.send(b"reset\r")
    try:
        tel.extend(drain(conn, 4.0))
        conn.send(b"exit\r")
        tel.extend(drain(conn, 2.0))
    except Exception as exc:
        print(f"reset telnet: {exc}; reconnect", flush=True)
        time.sleep(3.0)
        for attempt in range(20):
            try:
                conn = TelnetConnection(HOST, 23, timeout=2.0)
                conn.connect()
                break
            except Exception as e2:
                print(f"re {attempt}: {e2}", flush=True)
                time.sleep(2.0)
        conn.send(b"\r")
        drain(conn, 1.0)

    print("=== wait ':' / CR ===", flush=True)
    deadline = time.monotonic() + 90.0
    saw = False
    while time.monotonic() < deadline:
        try:
            chunk = conn.receive()
        except BenchmarkError:
            chunk = b""
        if chunk:
            tel.extend(chunk)
            sys.stdout.write(chunk.decode("latin-1", errors="replace"))
            sys.stdout.flush()
        window = (tel[-400:] + shared[-400:]).replace(b"\r", b"\n")
        for line in window.decode("latin-1", errors="replace").splitlines()[-15:]:
            if line.strip() == ":":
                saw = True
                break
        if saw:
            break
        time.sleep(0.05)

    print("\n=== CR ===", flush=True)
    conn.send(b"\r")

    print("=== wait user mem ===", flush=True)
    deadline = time.monotonic() + 90.0
    saw_mem = False
    while time.monotonic() < deadline:
        try:
            chunk = conn.receive()
        except BenchmarkError:
            chunk = b""
        if chunk:
            tel.extend(chunk)
            sys.stdout.write(chunk.decode("latin-1", errors="replace"))
            sys.stdout.flush()
            if b"user mem" in tel.lower():
                saw_mem = True
                break
        time.sleep(0.02)

    if not saw_mem:
        print("WARNING: no user mem; continuing anyway", flush=True)
    else:
        # Let a few more chars finish the banner line
        tel.extend(drain(conn, 0.4))

    print("\n=== pause ASAP after user mem ===", flush=True)
    conn.send(b"\x1b>>")
    tel.extend(drain(conn, 2.0))
    mon(conn, "monitor", 1.5)
    mon(conn, "P", 2.0)
    mon(conn, "U", 3.0)

    # Snapshot pre-fault PC samples briefly
    print("\n=== pre-fault PC samples ===", flush=True)
    for i in range(5):
        mon(conn, "C", 0.15)
        time.sleep(0.05)
        data = mon(conn, "P", 0.8)
        for line in data.decode("latin-1", errors="replace").splitlines():
            if "state: PC=" in line:
                print(f"pre{i+1}: {line.strip()[:140]}", flush=True)

    print(f"\n=== arm T {TRACE_N} + B004332 ===", flush=True)
    # Clear any old break, arm fresh
    mon(conn, "B clear", 0.3)
    mon(conn, f"T {TRACE_N}", 0.6)
    mon(conn, "B004332", 0.4)

    # Mark COM stream
    shared.clear()  # keep file growing; analysis uses full log — don't clear file
    # Actually keep full log; analysis scans all. Mark with a note only.
    mark_pos = len(shared)

    mon(conn, "C", 0.3)
    print("=== wait first trap @004332 (max 45s) ===", flush=True)
    deadline = time.monotonic() + 45.0
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
            if b"004332" in hit and (
                b"BREAK" in hit or b"stopped" in hit or b"breakpoint" in hit.lower()
            ):
                # BREAK message uses pc=004332
                if b"pc=004332" in hit.lower() or b"PC=004332" in hit or b"BREAK pc=004332" in hit:
                    break
            if b"BREAK pc=004332" in hit or b"kek BREAK pc=004332" in hit:
                break
        # Also detect via stopped at 004332 after pause spam
        time.sleep(0.05)
    else:
        print("no BREAK seen; force P", flush=True)

    time.sleep(0.3)
    hit.extend(drain(conn, 1.5))
    print("\n=== FIRST TRAP DUMP ===", flush=True)
    mon(conn, "P", 2.0)
    mon(conn, "U", 3.5)
    # Kernel stack around SP from state — dump common area
    mon(conn, "MD147540:147630", 2.0)
    mon(conn, "D077122:077160", 1.5)
    # History to COM
    mon(conn, "H", 0.5)
    time.sleep(1.0)

    # If still at trap, step a bit to see path; then RTT once for user PC confirm
    mon(conn, "B clear", 0.3)
    mon(conn, "T 0", 0.4)  # stop further flood if any left

    # Dump user stack if we can get to RTT with stack intact
    mon(conn, "B004160", 0.4)
    mon(conn, "C", 0.2)
    time.sleep(1.0)
    drain(conn, 1.0)
    mon(conn, "P", 1.5)
    mon(conn, "MD147550:147620", 2.0)
    mon(conn, "S", 0.6)
    mon(conn, "P", 1.2)
    mon(conn, "U", 2.5)
    # User stack at SP after RTT
    mon(conn, "MD175270:175350", 2.0)
    mon(conn, "B clear", 0.3)

    tel_log.write_bytes(tel + hit)
    stop.set()
    time.sleep(0.5)
    try:
        ser.close()
    except Exception:
        pass
    conn.close()

    print("\n=== TRACE ANALYSIS ===", flush=True)
    # Prefer post-mark portion but analyze full COM log file
    com_all = com_log.read_bytes()
    analyze_trace(com_all, analysis)
    print(f"com={com_log}\ntel={tel_log}\nmark_pos={mark_pos}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
