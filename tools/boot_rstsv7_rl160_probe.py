#!/usr/bin/env python3
"""Reset RSTS7, arm dl_trace, capture RL IRQ path around unexpected 000160."""

from __future__ import annotations

import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from benchmark_boot_times import TelnetConnection, BenchmarkError  # noqa: E402

HOST = "192.168.7.144"
COM = "COM18"
OUT = ROOT / "boot-benchmark-results-211bsd-postphys"


def main() -> int:
    import serial

    OUT.mkdir(exist_ok=True)
    ser = serial.Serial()
    ser.port = COM
    ser.baudrate = 115200
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    time.sleep(0.3)

    shared = bytearray()
    stop = threading.Event()

    def reader():
        while not stop.is_set():
            try:
                n = ser.in_waiting
                chunk = ser.read(max(1, min(n, 4096))) if n else ser.read(1)
            except Exception:
                break
            if chunk:
                shared.extend(chunk)

    thr = threading.Thread(target=reader, daemon=True)
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

    def drain(seconds: float) -> bytes:
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

    def send_line(s: str) -> None:
        conn.send(s.encode("ascii") + b"\r")

    def to_shell() -> None:
        send_line("")
        data = drain(1.0)
        if b"monitor>" in data:
            send_line(">")
            drain(1.0)
            return
        if b"vpdp:" in data or b"management shell" in data:
            return
        conn.send(b"\x1b>>")
        data = drain(3.0)
        if b"monitor>" in data:
            send_line(">")
            drain(1.0)

    to_shell()
    print("=== reset ===", flush=True)
    send_line("reset")
    drain(10.0)
    for _ in range(20):
        send_line("")
        data = drain(0.7)
        if b"vpdp:" in data:
            break
        time.sleep(0.2)

    print("=== arm dl_trace ===", flush=True)
    send_line("set dl_trace=800")
    drain(1.0)
    send_line("exit")
    drain(1.0)

    print("=== watch guest ===", flush=True)
    end = time.monotonic() + 50.0
    while time.monotonic() < end:
        try:
            chunk = conn.receive()
        except BenchmarkError:
            chunk = b""
        if chunk:
            sys.stdout.write(chunk.decode("latin-1", errors="replace"))
            sys.stdout.flush()
            low = chunk.lower()
            if b"fatal" in low or b"option:" in low or b"000160" in chunk:
                time.sleep(1.0)
                drain(1.0)
                break
        time.sleep(0.02)

    conn.send(b"\x1b>>")
    drain(2.0)
    to_shell()
    send_line("monitor")
    drain(1.0)
    send_line("P")
    drain(2.0)
    send_line("rl")
    drain(2.0)
    send_line(">")
    drain(0.5)

    stop.set()
    thr.join(timeout=2)
    ser.close()
    conn.close()

    text = shared.decode("latin-1", errors="replace")
    log = OUT / "rstsv7-rl160-com-trace.log"
    log.write_text(text, encoding="latin-1", errors="replace")
    print(f"\n=== COM trace -> {log} ===", flush=True)
    n = 0
    for line in text.splitlines():
        if any(
            k in line
            for k in (
                "IRQ",
                "WRITE CSR",
                "DEFER",
                "GETSTAT",
                "READ-",
                "SEEK",
                "RDHDR",
                "NOT-ATTACHED",
                "NOIRQ",
            )
        ):
            print(line, flush=True)
            n += 1
            if n >= 120:
                break
    print(f"=== printed {n} lines ===", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
