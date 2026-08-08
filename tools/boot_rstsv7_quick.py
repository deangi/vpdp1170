#!/usr/bin/env python3
"""Reset board via brief COM open, then telnet-only RSTS7 boot watch."""

from __future__ import annotations

import socket
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from benchmark_boot_times import BenchmarkError, TelnetConnection  # noqa: E402

HOST = "192.168.7.144"
COM = "COM18"
OUT = ROOT / "boot-benchmark-results-211bsd-postphys"


def usb_reset() -> None:
    import serial

    ser = serial.Serial()
    ser.port = COM
    ser.baudrate = 115200
    ser.timeout = 0.2
    # Opening CDC often resets ESP32-S3; keep it brief.
    ser.open()
    time.sleep(0.4)
    ser.close()
    print("COM opened/closed (USB reset)", flush=True)


def wait_port(port: int, tries: int = 45) -> None:
    for i in range(tries):
        s = socket.socket()
        s.settimeout(1.5)
        try:
            s.connect((HOST, port))
            s.close()
            print(f"port {port} open after {i}", flush=True)
            return
        except OSError as exc:
            print(f"wait {port}: {i} {type(exc).__name__}", flush=True)
            time.sleep(2)
    raise SystemExit(f"port {port} never opened")


def main() -> int:
    OUT.mkdir(exist_ok=True)
    usb_reset()
    time.sleep(3)
    wait_port(23)

    conn = TelnetConnection(HOST, 23, timeout=2.0)
    conn.connect()

    def drain(sec: float) -> bytes:
        data = bytearray()
        end = time.monotonic() + sec
        while time.monotonic() < end:
            try:
                chunk = conn.receive()
            except BenchmarkError:
                break
            if chunk:
                data.extend(chunk)
                sys.stdout.write(chunk.decode("latin-1", errors="replace"))
                sys.stdout.flush()
        return bytes(data)

    def send(s: str) -> None:
        conn.send(s.encode("ascii") + b"\r")

    send("")
    data = drain(1.5)
    if b"monitor>" in data:
        send(">")
        drain(1.0)
    elif b"vpdp:" not in data:
        conn.send(b"\x1b")
        time.sleep(0.08)
        conn.send(b">>")
        data = drain(2.5)
        if b"monitor>" in data:
            send(">")
            drain(1.0)

    print("=== reset ===", flush=True)
    send("reset")
    drain(12.0)
    for _ in range(20):
        send("")
        if b"vpdp:" in drain(0.5):
            break

    send("exit")
    drain(0.8)
    print("=== watch ===", flush=True)
    end = time.monotonic() + 55.0
    got = bytearray()
    while time.monotonic() < end:
        try:
            chunk = conn.receive()
        except BenchmarkError:
            chunk = b""
        if chunk:
            got.extend(chunk)
            sys.stdout.write(chunk.decode("latin-1", errors="replace"))
            sys.stdout.flush()
            low = chunk.lower()
            if b"fatal" in low or b"option:" in low or b"does not interrupt" in low:
                time.sleep(2.0)
                drain(2.0)
                break
        time.sleep(0.02)

    conn.close()
    text = got.decode("latin-1", errors="replace")
    log = OUT / "rstsv7-quick.log"
    log.write_text(text, encoding="latin-1", errors="replace")
    print("\n=== summary ===", flush=True)
    for key in (
        "does not interrupt",
        "Unexpected",
        "Option:",
        "Fatal",
        "KW11-P",
        "Device RL",
        "Device RB",
    ):
        print(f"{key!r}: {text.count(key)}", flush=True)
    print(f"log {log}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
