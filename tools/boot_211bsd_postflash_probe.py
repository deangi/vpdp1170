#!/usr/bin/env python3
"""Post-flash 211BSD: COM open, EN reset, CR at ':', hang snapshot."""

from __future__ import annotations

import socket
import sys
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from benchmark_boot_times import TelnetConnection, BenchmarkError  # noqa: E402

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


def wait_tcp(port: int = 23, timeout: float = 90.0) -> None:
    deadline = time.monotonic() + timeout
    n = 0
    while time.monotonic() < deadline:
        n += 1
        s = socket.socket()
        s.settimeout(2.0)
        try:
            s.connect((HOST, port))
            s.close()
            print(f"tcp/{port} up (try {n})", flush=True)
            return
        except Exception as exc:
            print(f"tcp/{port} wait {n}: {exc}", flush=True)
            time.sleep(2.0)
    raise RuntimeError(f"tcp/{port} never came up")


def open_com():
    import serial

    ser = serial.Serial()
    ser.port = COM
    ser.baudrate = 115200
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def en_reset(ser) -> None:
    print("=== EN/RTS reset (DTR held low) ===", flush=True)
    ser.dtr = False
    ser.rts = False
    time.sleep(0.05)
    ser.rts = True
    time.sleep(0.2)
    ser.rts = False
    ser.dtr = False


def main() -> int:
    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    com_log = OUT / f"{stamp}-postflash-com.log"
    tel_log = OUT / f"{stamp}-postflash-tel.log"
    com_buf = bytearray()
    tel_buf = bytearray()

    print(f"=== open {COM} ===", flush=True)
    ser = open_com()
    time.sleep(0.4)
    en_reset(ser)

    print("=== wait for ESP boot on COM (up to 45s) ===", flush=True)
    t0 = time.monotonic()
    with com_log.open("wb") as f:
        while time.monotonic() - t0 < 45.0:
            try:
                data = ser.read(8192)
            except Exception as exc:
                print(f"COM err: {exc}", flush=True)
                time.sleep(1.0)
                try:
                    ser.close()
                except Exception:
                    pass
                time.sleep(1.0)
                ser = open_com()
                continue
            if data:
                com_buf.extend(data)
                f.write(data)
                f.flush()
                sys.stdout.write(data.decode("latin-1", errors="replace"))
                sys.stdout.flush()
                low = com_buf.lower()
                if b"telnet" in low and (
                    b"listening" in low or b"started" in low or b"ready" in low
                ):
                    break
                if b"adapter console ok" in low:
                    break

    print("\n=== wait telnet service ===", flush=True)
    wait_tcp(23, 90.0)
    time.sleep(2.0)

    conn = TelnetConnection(HOST, 23, timeout=2.0)
    for attempt in range(10):
        try:
            conn.connect()
            break
        except Exception as exc:
            print(f"telnet connect {attempt}: {exc}", flush=True)
            time.sleep(2.0)
            conn = TelnetConnection(HOST, 23, timeout=2.0)
    else:
        raise RuntimeError("telnet connect failed")

    # Ensure guest console
    conn.send(b"\r")
    data = drain(conn, 1.5)
    tel_buf.extend(data)
    if b"monitor>" in data:
        mon(conn, "C", 0.3)
        mon(conn, ">", 0.4)
        conn.send(b"exit\r")
        tel_buf.extend(drain(conn, 1.5))
    elif b"vpdp:" in data:
        conn.send(b"exit\r")
        tel_buf.extend(drain(conn, 1.5))

    print("=== wait for ':' ===", flush=True)
    deadline = time.monotonic() + 75.0
    saw_colon = False
    while time.monotonic() < deadline:
        try:
            chunk = conn.receive()
        except BenchmarkError:
            chunk = b""
        if chunk:
            tel_buf.extend(chunk)
            sys.stdout.write(chunk.decode("latin-1", errors="replace"))
            sys.stdout.flush()
        window = (tel_buf[-400:] + com_buf[-400:]).replace(b"\r", b"\n")
        for line in window.decode("latin-1", errors="replace").splitlines()[-15:]:
            if line.strip() == ":":
                saw_colon = True
                break
        if saw_colon:
            break
        time.sleep(0.05)

    if saw_colon:
        print("\n=== send CR at ':' ===", flush=True)
    else:
        print("\n=== no ':' seen; send CR anyway ===", flush=True)
    conn.send(b"\r")

    print("=== boot capture ===", flush=True)
    t0 = time.monotonic()
    last = time.monotonic()
    saw_usermem = False
    while time.monotonic() - t0 < 100.0:
        try:
            chunk = conn.receive()
        except BenchmarkError:
            chunk = b""
        if chunk:
            tel_buf.extend(chunk)
            last = time.monotonic()
            sys.stdout.write(chunk.decode("latin-1", errors="replace"))
            sys.stdout.flush()
            low = tel_buf.lower()
            if b"user mem" in low:
                saw_usermem = True
            if b"login:" in low or b"\n# " in tel_buf:
                print("\n=== reached login/# ===", flush=True)
                break
            if b"configure" in low:
                print("\n[configure]", flush=True)
        else:
            time.sleep(0.05)
            if saw_usermem and time.monotonic() - last >= 20.0:
                print("\n=== hang: quiet after user mem ===", flush=True)
                break

    tel_log.write_bytes(tel_buf)

    print("\n=== monitor snapshot ===", flush=True)
    conn.send(b"\x1b>>")
    drain(conn, 2.5)
    if True:
        mon(conn, "monitor", 2.0)
    mon(conn, "P", 2.5)
    mon(conn, "U", 3.5)
    print("\n=== PC samples ===", flush=True)
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
    print("trap vec" if b"004332" in hit else "no trap msg", flush=True)
    mon(conn, "P", 1.5)
    mon(conn, "U", 3.0)
    mon(conn, "D077122:077150", 1.5)
    mon(conn, "B clear", 0.3)

    print("\n=== RTT -> user PC ===", flush=True)
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
    print("\n>>> rl regs", flush=True)
    conn.send(b"rl regs\r")
    drain(conn, 3.0)
    print("\n>>> clock", flush=True)
    conn.send(b"clock\r")
    drain(conn, 2.0)

    try:
        ser.close()
    except Exception:
        pass
    conn.close()
    print(f"\ncom={com_log}\ntel={tel_log}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
