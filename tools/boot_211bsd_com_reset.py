#!/usr/bin/env python3
"""211BSD boot via COM18 open + hard reset + CR at ':' prompt.

Opening COM18 can leave the board half-reset. Pulse DTR/RTS for a proper
ESP32 reset (drops Telnet), wait for services, reconnect, answer ':' with CR,
then capture until hang / login and snapshot monitor state.
"""

from __future__ import annotations

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
BOOT_WAIT_SECS = 120.0
HANG_QUIET_SECS = 25.0


def drain(conn, seconds):
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


def mon(conn, cmd, wait=1.8):
    print(f"\n>>> {cmd}", flush=True)
    conn.send(cmd.encode() + b"\r")
    return drain(conn, wait)


def open_serial():
    import serial

    # Open without asserting DTR/RTS (avoids accidental reset timing races).
    device = serial.Serial()
    device.port = COM
    device.baudrate = 115200
    device.timeout = 0.2
    device.dtr = False
    device.rts = False
    device.open()
    return device


def hard_reset(ser):
    """Proper ESP32 reset after COM open left the board half-dead.

    Pulse EN (RTS) only; keep DTR deasserted so GPIO0 is not held for
    download mode (earlier DTR+RTS True briefly entered 'waiting for download').
    """
    print("=== hard reset via RTS/EN (DTR clear) ===", flush=True)
    ser.dtr = False
    ser.rts = False
    time.sleep(0.05)
    ser.rts = True  # EN low = reset
    time.sleep(0.15)
    ser.rts = False  # EN high = run
    ser.dtr = False


def serial_reader(ser, stop, log_path, shared):
    with log_path.open("wb") as f:
        while not stop.is_set():
            try:
                data = ser.read(8192)
            except Exception as exc:
                print(f"COM read err: {exc}", flush=True)
                break
            if data:
                f.write(data)
                f.flush()
                shared.extend(data)
                sys.stdout.write(data.decode("latin-1", errors="replace"))
                sys.stdout.flush()


def wait_telnet(timeout=90.0):
    deadline = time.monotonic() + timeout
    attempt = 0
    while time.monotonic() < deadline:
        attempt += 1
        conn = TelnetConnection(HOST, 23, timeout=2.0)
        try:
            conn.connect()
            print(f"telnet up (attempt {attempt})", flush=True)
            return conn
        except Exception as exc:
            print(f"telnet wait {attempt}: {exc}", flush=True)
            time.sleep(2.0)
    raise RuntimeError("telnet did not come back after reset")


def to_monitor(conn):
    conn.send(b"\r")
    data = drain(conn, 1.0)
    if b"monitor>" in data:
        return
    if b"vpdp:" in data:
        mon(conn, "monitor", 2.0)
        return
    conn.send(b"\x1b>>")
    data = drain(conn, 2.5)
    if b"monitor>" in data:
        return
    if b"vpdp:" in data or b"management" in data:
        mon(conn, "monitor", 2.0)
        return
    # last resort: already guest; try again
    conn.send(b"\x1b>>")
    drain(conn, 2.5)
    mon(conn, "monitor", 2.0)


def main() -> int:
    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    ser_log = OUT / f"{stamp}-com18-boot.log"
    tel_log = OUT / f"{stamp}-telnet-boot.log"
    snap_log = OUT / f"{stamp}-posthang-snap.log"
    shared = bytearray()
    tel_buf = bytearray()

    print(f"=== open {COM} ===", flush=True)
    ser = open_serial()
    time.sleep(0.3)
    hard_reset(ser)
    # Serial may drop during reset; reopen if needed.
    try:
        _ = ser.in_waiting
    except Exception:
        try:
            ser.close()
        except Exception:
            pass
        time.sleep(2.0)
        ser = open_serial()

    stop = threading.Event()
    thr = threading.Thread(
        target=serial_reader, args=(ser, stop, ser_log, shared), daemon=True
    )
    thr.start()

    print("=== wait for WiFi/Telnet after reset ===", flush=True)
    time.sleep(8.0)
    conn = wait_telnet(100.0)

    # Ensure guest console on this telnet session
    conn.send(b"\r")
    data = drain(conn, 1.5)
    tel_buf.extend(data)
    if b"vpdp:" in data or b"monitor>" in data:
        if b"monitor>" in data:
            mon(conn, "C", 0.3)
            mon(conn, ">", 0.4)
        print(">>> exit (to guest)", flush=True)
        conn.send(b"exit\r")
        tel_buf.extend(drain(conn, 1.5))

    print("=== wait for ':' boot prompt ===", flush=True)
    saw_colon = False
    deadline = time.monotonic() + 90.0
    while time.monotonic() < deadline:
        # Prefer guest text on telnet; COM also has host logs + guest.
        try:
            chunk = conn.receive()
        except BenchmarkError:
            chunk = b""
        if chunk:
            tel_buf.extend(chunk)
            sys.stdout.write(chunk.decode("latin-1", errors="replace"))
            sys.stdout.flush()
        # Standalone boot prints ":" (often after "Boot from rl...")
        window = (tel_buf[-200:] + shared[-200:]).replace(b"\r", b"\n")
        if b"\n: " in window or window.endswith(b"\n:") or window.endswith(b": "):
            # Avoid matching timestamps / IPv6 etc. — look for short line ':'
            text = window.decode("latin-1", errors="replace")
            for line in text.splitlines()[-8:]:
                if line.strip() == ":":
                    saw_colon = True
                    break
            if saw_colon:
                break
        time.sleep(0.05)

    if not saw_colon:
        print("WARNING: no ':' seen; sending CR anyway", flush=True)
    else:
        print("=== seen ':' — send CR ===", flush=True)
    conn.send(b"\r")

    print(f"=== capture boot (max {BOOT_WAIT_SECS:.0f}s) ===", flush=True)
    t0 = time.monotonic()
    last_guest = time.monotonic()
    saw_usermem = False
    while time.monotonic() - t0 < BOOT_WAIT_SECS:
        try:
            chunk = conn.receive()
        except BenchmarkError:
            chunk = b""
        if chunk:
            tel_buf.extend(chunk)
            sys.stdout.write(chunk.decode("latin-1", errors="replace"))
            sys.stdout.flush()
            last_guest = time.monotonic()
            low = tel_buf.lower()
            if b"user mem" in low:
                saw_usermem = True
            if b"login:" in low or b"\n# " in tel_buf or tel_buf.endswith(b"# "):
                print("\n=== reached login/# ===", flush=True)
                break
            if b"configure" in low:
                print("\n[seen configure]", flush=True)
        else:
            time.sleep(0.05)
            if saw_usermem and (time.monotonic() - last_guest) >= HANG_QUIET_SECS:
                print(
                    f"\n=== quiet {HANG_QUIET_SECS:.0f}s after user mem — treating as hang ===",
                    flush=True,
                )
                break

    tel_log.write_bytes(tel_buf)
    print(f"\n=== hang/idle — snapshot monitor ===", flush=True)
    try:
        to_monitor(conn)
        snap = bytearray()
        snap.extend(mon(conn, "P", 2.5))
        snap.extend(mon(conn, "U", 3.5))
        print("\n=== PC samples ===\n", flush=True)
        for i in range(10):
            snap.extend(mon(conn, "C", 0.2))
            time.sleep(0.12)
            data = mon(conn, "P", 1.0)
            snap.extend(data)
            for line in data.decode("latin-1", errors="replace").splitlines():
                if "state: PC=" in line:
                    print(f"s{i+1}: {line.strip()[:140]}", flush=True)
        snap.extend(mon(conn, "B004332", 0.4))
        snap.extend(mon(conn, "C", 0.2))
        time.sleep(2.0)
        hit = drain(conn, 1.5)
        snap.extend(hit)
        print(
            "trap hit" if (b"stopped" in hit and b"004332" in hit) else "no trap in 2s",
            flush=True,
        )
        snap.extend(mon(conn, "P", 1.5))
        snap.extend(mon(conn, "B clear", 0.4))
        snap.extend(mon(conn, ">", 0.4))
        print("\n>>> rl regs", flush=True)
        conn.send(b"rl regs\r")
        snap.extend(drain(conn, 3.0))
        print("\n>>> clock", flush=True)
        conn.send(b"clock\r")
        snap.extend(drain(conn, 2.0))
        print("\n>>> tty", flush=True)
        conn.send(b"tty\r")
        snap.extend(drain(conn, 2.0))
        snap_log.write_bytes(snap)
    except Exception as exc:
        print(f"snapshot failed: {exc}", flush=True)

    stop.set()
    time.sleep(0.5)
    try:
        ser.close()
    except Exception:
        pass
    print(f"\nser={ser_log}\ntel={tel_log}\nsnap={snap_log}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
