#!/usr/bin/env python3
"""Boot to phys-mem hang; arm dl_trace AFTER reset; capture COM18."""

from __future__ import annotations

import sys
import threading
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from benchmark_boot_times import (  # noqa: E402
    BootProfile,
    TelnetConnection,
    BenchmarkError,
    SHELL_PROMPT_RE,
    SHELL_BANNER,
    install_config,
    shell_command,
)

HOST = "192.168.7.144"
COM = "COM18"
OUT = ROOT / "boot-benchmark-results-211bsd-postphys"


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
    raise RuntimeError("no shell")


def sh(conn: TelnetConnection, cmd: str, wait: float = 1.5) -> bytes:
    conn.send(cmd.encode("ascii") + b"\r")
    return drain(conn, wait)


def serial_reader(stop: threading.Event, log_path: Path) -> None:
    import serial

    ser = serial.Serial(COM, 115200, timeout=0.2)
    with log_path.open("wb") as f:
        while not stop.is_set():
            data = ser.read(4096)
            if data:
                f.write(data)
                f.flush()
                # Don't echo all serial to stdout (pcping noise); keep file only.
    ser.close()


def main() -> int:
    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    telnet_log = OUT / f"{stamp}-telnet.log"
    serial_log = OUT / f"{stamp}-com18.log"

    profile = BootProfile(
        name="211bsd",
        config_path="/pdpconfig-211bsd.ini",
        completion=b"login: ",
        quiet_seconds=2.0,
    )
    conn = TelnetConnection(HOST, 23, timeout=2.0)
    for attempt in range(30):
        try:
            conn.connect()
            break
        except Exception as exc:
            print(f"telnet wait {attempt}: {exc}", flush=True)
            time.sleep(2)
            conn = TelnetConnection(HOST, 23, timeout=2.0)
    else:
        raise RuntimeError("telnet never became available")

    stop = threading.Event()
    thr = threading.Thread(target=serial_reader, args=(stop, serial_log), daemon=True)
    thr.start()
    time.sleep(0.3)

    ensure_shell(conn)
    install_config(conn, profile, 12.0, True)
    shell_command(conn, "set pcping=0", 1.0, True)
    shell_command(conn, "set break=0", 1.0, True)

    print("\n=== reset + exit ===\n", flush=True)
    conn.send(b"reset\r")
    time.sleep(0.15)
    conn.send(b"exit\r")
    drain(conn, 1.0)

    buf = bytearray()
    start = time.monotonic()
    last_out = start
    last_cr = 0.0
    cr_n = 0
    phys_at = None
    armed_trace = False
    deadline = start + 240.0

    print("\n=== capture ===\n", flush=True)
    while time.monotonic() < deadline:
        now = time.monotonic()
        if (
            cr_n < 25
            and now - start < 50
            and b"2.11 BSD" not in buf
            and now - last_cr >= 2.0
        ):
            conn.send(b"\r")
            last_cr = now
            cr_n += 1

        try:
            chunk = conn.receive()
        except BenchmarkError:
            time.sleep(0.05)
            continue

        if chunk:
            buf.extend(chunk)
            last_out = now
            sys.stdout.write(chunk.decode("latin-1", errors="replace"))
            sys.stdout.flush()
            # Arm as soon as the kernel banner appears so post-phys I/O is traced.
            if (b"2.11 BSD" in buf or b"phys mem" in buf) and not armed_trace:
                print("\n*** banner/phys — arming dl_trace ***\n", flush=True)
                ensure_shell(conn)
                sh(conn, "set pcping=0", 1.0)
                sh(conn, "set dl_trace=1000", 1.0)
                sh(conn, "exit", 0.5)
                armed_trace = True
                last_out = time.monotonic()
            if b"phys mem" in buf and phys_at is None:
                phys_at = now
                print("\n*** phys mem ***\n", flush=True)
            if b"login: " in buf or b"configure system" in buf:
                break
            continue

        if phys_at is not None and armed_trace and now - last_out > 25.0:
            print("\n*** quiet after trace arm — dump ***\n", flush=True)
            break
        if phys_at is not None and not armed_trace and now - last_out > 40.0:
            print("\n*** quiet before arm — dump ***\n", flush=True)
            break
        if not buf and now - start > 90:
            break

    telnet_log.write_bytes(buf)
    time.sleep(1.5)

    ensure_shell(conn)
    print("\n=== rl regs ===\n", flush=True)
    sh(conn, "rl regs", 2.5)
    sh(conn, "tty", 2.0)
    sh(conn, "set dl_trace=0", 1.0)
    sh(conn, "exit", 0.5)

    stop.set()
    thr.join(timeout=2.0)
    conn.close()

    text = serial_log.read_text(encoding="latin-1", errors="replace")
    keys = (
        "IRQ-SCHEDULE",
        "IRQ-DELIVER",
        "IRQ-CANCEL",
        "NOIRQ",
        "DEFER-DATA",
        "DEFER-COMPLETE",
        "WRITE CSR",
        "WRITE-BUSY",
        "GETSTAT",
        "SEEK",
        "READ-DONE",
        "WRITE-DONE",
    )
    print("\n=== COM18 IRQ/defer summary ===\n", flush=True)
    for k in keys:
        n = text.count(k)
        if n:
            print(f"  {k}: {n}", flush=True)
    lines = [ln for ln in text.splitlines() if any(k in ln for k in keys)]
    print("\n--- last 50 RL trace lines ---", flush=True)
    for ln in lines[-50:]:
        print(ln, flush=True)

    print(f"\ntelnet: {telnet_log}\ncom18: {serial_log} ({len(text)} chars)", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
