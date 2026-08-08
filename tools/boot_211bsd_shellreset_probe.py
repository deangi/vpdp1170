#!/usr/bin/env python3
"""Investigate 211BSD hang: keep COM18 open, shell reset, CR at ':', snapshot."""

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
    enter_shell,
)

HOST = "192.168.7.144"
COM = "COM18"
OUT = ROOT / "boot-benchmark-results-211bsd-postphys"


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


def mon(conn, cmd: str, wait: float = 1.8) -> bytes:
    print(f"\n>>> {cmd}", flush=True)
    conn.send(cmd.encode() + b"\r")
    return drain(conn, wait)


def serial_reader(ser, stop, log_path, shared):
    with log_path.open("wb") as f:
        while not stop.is_set():
            try:
                data = ser.read(8192)
            except Exception as exc:
                print(f"COM err: {exc}", flush=True)
                break
            if data:
                shared.extend(data)
                f.write(data)
                f.flush()
                sys.stdout.write(data.decode("latin-1", errors="replace"))
                sys.stdout.flush()


def main() -> int:
    import serial

    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    com_log = OUT / f"{stamp}-shellreset-com.log"
    tel_log = OUT / f"{stamp}-shellreset-tel.log"
    shared = bytearray()
    tel = bytearray()

    print(f"=== open {COM} (dtr/rts low) ===", flush=True)
    ser = serial.Serial()
    ser.port = COM
    ser.baudrate = 115200
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    time.sleep(0.5)

    # User guidance: opening COM can leave board odd — pulse EN, then use
    # management-shell reset for a definitive PDP cold boot.
    print("=== EN pulse ===", flush=True)
    ser.dtr = False
    ser.rts = True
    time.sleep(0.15)
    ser.rts = False
    ser.dtr = False
    time.sleep(1.0)

    stop = threading.Event()
    thr = threading.Thread(
        target=serial_reader, args=(ser, stop, com_log, shared), daemon=True
    )
    thr.start()

    print("=== telnet / management shell ===", flush=True)
    conn = None
    for attempt in range(20):
        try:
            conn = TelnetConnection(HOST, 23, timeout=2.0)
            conn.connect()
            print(f"telnet connected try {attempt}", flush=True)
            break
        except Exception as exc:
            print(f"telnet wait {attempt}: {exc}", flush=True)
            time.sleep(2.0)
    if conn is None:
        raise RuntimeError("telnet down")

    # Get to shell prompt cleanly
    conn.send(b"\r")
    data = drain(conn, 1.0)
    tel.extend(data)
    if b"monitor>" in data:
        mon(conn, ">", 0.5)
    elif b"vpdp:" not in data:
        try:
            enter_shell(conn, 15.0, True)
        except Exception:
            conn.send(b"\x1b>>")
            drain(conn, 2.5)

    # Clear any garbage line
    conn.send(b"\r")
    drain(conn, 0.5)

    print("=== shell reset (PDP cold boot) ===", flush=True)
    conn.send(b"reset\r")
    # reset drops telnet on some builds; tolerate that
    try:
        tel.extend(drain(conn, 3.0))
        conn.send(b"exit\r")
        tel.extend(drain(conn, 2.0))
    except Exception as exc:
        print(f"telnet after reset: {exc}; reconnecting", flush=True)
        time.sleep(3.0)
        for attempt in range(20):
            try:
                conn = TelnetConnection(HOST, 23, timeout=2.0)
                conn.connect()
                break
            except Exception as e2:
                print(f"reconnect {attempt}: {e2}", flush=True)
                time.sleep(2.0)
        else:
            raise RuntimeError("telnet lost after reset")
        conn.send(b"\r")
        drain(conn, 1.0)

    print("=== wait ':' ===", flush=True)
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
        window = (tel[-500:] + shared[-500:]).replace(b"\r", b"\n")
        for line in window.decode("latin-1", errors="replace").splitlines()[-20:]:
            if line.strip() == ":":
                saw = True
                break
        if saw:
            break
        time.sleep(0.05)

    print("\n=== CR ===" if saw else "\n=== CR (no ':' seen) ===", flush=True)
    try:
        conn.send(b"\r")
    except Exception as exc:
        print(f"send CR failed: {exc}", flush=True)
        raise

    print("=== capture ===", flush=True)
    t0 = time.monotonic()
    last = time.monotonic()
    usermem = False
    while time.monotonic() - t0 < 100.0:
        try:
            chunk = conn.receive()
        except BenchmarkError:
            chunk = b""
        if chunk:
            tel.extend(chunk)
            last = time.monotonic()
            sys.stdout.write(chunk.decode("latin-1", errors="replace"))
            sys.stdout.flush()
            low = tel.lower()
            if b"user mem" in low:
                usermem = True
            if b"login:" in low or b"\n# " in tel:
                print("\n=== login/# ===", flush=True)
                break
            if b"configure" in low:
                print("\n[configure]", flush=True)
        else:
            time.sleep(0.05)
            if usermem and time.monotonic() - last >= 20.0:
                print("\n=== hang after user mem ===", flush=True)
                break

    tel_log.write_bytes(tel)

    print("\n=== snapshot ===", flush=True)
    conn.send(b"\x1b>>")
    drain(conn, 2.5)
    mon(conn, "monitor", 2.0)
    mon(conn, "P", 2.5)
    mon(conn, "U", 3.5)
    for i in range(8):
        mon(conn, "C", 0.2)
        time.sleep(0.12)
        data = mon(conn, "P", 1.0)
        for line in data.decode("latin-1", errors="replace").splitlines():
            if "state: PC=" in line:
                print(f"s{i+1}: {line.strip()[:150]}", flush=True)

    mon(conn, "B004332", 0.4)
    mon(conn, "C", 0.2)
    time.sleep(1.5)
    hit = drain(conn, 1.5)
    print("trap" if b"004332" in hit else "notrap-msg", flush=True)
    mon(conn, "P", 1.5)
    mon(conn, "U", 3.0)
    mon(conn, "D077122:077150", 1.5)
    mon(conn, "B clear", 0.3)

    mon(conn, "B004160", 0.4)
    mon(conn, "C", 0.2)
    time.sleep(0.8)
    drain(conn, 1.0)
    mon(conn, "P", 1.5)
    mon(conn, "U", 2.5)
    mon(conn, "S", 0.7)
    mon(conn, "P", 1.2)
    mon(conn, "U", 2.5)
    mon(conn, "B clear", 0.3)

    mon(conn, ">", 0.4)
    conn.send(b"rl regs\r")
    drain(conn, 3.0)
    conn.send(b"clock\r")
    drain(conn, 2.0)

    stop.set()
    time.sleep(0.4)
    try:
        ser.close()
    except Exception:
        pass
    conn.close()
    print(f"\ncom={com_log}\ntel={tel_log}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
