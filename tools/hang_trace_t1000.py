#!/usr/bin/env python3
"""Board already in 211bsd hang: dump state, T 1000 to COM18, save logs."""

from __future__ import annotations

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
TRACE_N = 1000


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
    raise RuntimeError("no management shell")


def sh(conn: TelnetConnection, cmd: str, wait: float = 1.5) -> bytes:
    print(f"\n>>> {cmd}", flush=True)
    conn.send(cmd.encode("ascii") + b"\r")
    return drain(conn, wait)


def mon(conn: TelnetConnection, cmd: str, wait: float = 1.2) -> bytes:
    print(f"\n>>> mon {cmd}", flush=True)
    conn.send(cmd.encode("ascii") + b"\r")
    return drain(conn, wait)


def serial_reader(stop: threading.Event, log_path: Path) -> None:
    import serial

    ser = serial.Serial(COM, 115200, timeout=0.2)
    with log_path.open("wb") as f:
        while not stop.is_set():
            data = ser.read(8192)
            if data:
                f.write(data)
                f.flush()
                sys.stderr.buffer.write(data)
                sys.stderr.buffer.flush()
    ser.close()


def main() -> int:
    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    serial_log = OUT / f"{stamp}-hang-T{TRACE_N}-com18.log"
    telnet_log = OUT / f"{stamp}-hang-T{TRACE_N}-telnet.log"

    stop = threading.Event()
    thr = threading.Thread(target=serial_reader, args=(stop, serial_log), daemon=True)
    thr.start()
    time.sleep(0.5)

    conn = TelnetConnection(HOST, 23, timeout=2.0)
    conn.connect()
    ensure_shell(conn)
    telnet_buf = bytearray()

    def capture(data: bytes) -> None:
        telnet_buf.extend(data)

    print("=== pre-trace hang dump ===", flush=True)
    for cmd in ("rl regs", "lights", "tty"):
        capture(sh(conn, cmd, 2.0))

    capture(sh(conn, "monitor", 0.8))
    capture(mon(conn, "P", 2.0))
    for cmd, w in (
        ("U", 2.5),
        ("D077100", 1.2),
        ("M026000", 1.2),
        ("M003100", 1.2),
    ):
        capture(mon(conn, cmd, w))

    print(f"\n=== arm T {TRACE_N}, continue, capture COM18 ===\n", flush=True)
    capture(mon(conn, f"T {TRACE_N}", 0.5))
    capture(mon(conn, "C", 0.3))
    # Let ~1000 instructions run; hang is tight so this is quick wall-time,
    # but allow slack for USB logging.
    time.sleep(8.0)

    capture(sh(conn, "monitor", 0.5) if False else b"")  # still in monitor
    capture(mon(conn, "P", 2.0))
    capture(mon(conn, "T 0", 0.4))
    for cmd in ("rl regs",):
        # leave monitor first for rl regs
        pass
    capture(mon(conn, ">", 0.4))
    capture(sh(conn, "rl regs", 2.0))
    capture(sh(conn, "exit", 0.5))

    stop.set()
    thr.join(timeout=3)
    conn.close()
    telnet_log.write_bytes(telnet_buf)

    text = serial_log.read_text(encoding="latin-1", errors="replace")
    lines = text.splitlines()
    trace_lines = [ln for ln in lines if "kek trace:" in ln or "kek HALT" in ln or "kek PDP" in ln]
    print(f"\n=== COM18 summary: {len(lines)} lines, {len(trace_lines)} trace-ish ===", flush=True)
    for ln in trace_lines[:30]:
        print(ln, flush=True)
    if len(trace_lines) > 60:
        print("...", flush=True)
    for ln in trace_lines[-20:]:
        print(ln, flush=True)

    # PC histogram from trace lines
    pcs = []
    for ln in trace_lines:
        if "PC=" in ln:
            try:
                i = ln.index("PC=")
                pcs.append(ln[i + 3 : i + 9])
            except Exception:
                pass
    if pcs:
        from collections import Counter

        top = Counter(pcs).most_common(15)
        print("\n=== top PCs in trace ===", flush=True)
        for pc, n in top:
            print(f"  {pc}  x{n}", flush=True)

    print(f"\ntelnet={telnet_log}\ncom18={serial_log}", flush=True)
    return 0 if trace_lines else 1


if __name__ == "__main__":
    raise SystemExit(main())
