#!/usr/bin/env python3
"""FTP fresh 211bsd ini; boot; on hang sample clock via COM18 + code dump."""

from __future__ import annotations

import sys
import threading
import time
from datetime import datetime
from ftplib import FTP
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
LOCAL_INI = ROOT / "PdpSdCard" / "pdpconfig-211bsd.ini"
OUT = ROOT / "boot-benchmark-results-211bsd-postphys"


def ftp_upload() -> None:
    creds = [("esp32", "esp32"), ("anonymous", ""), ("anonymous", "anonymous")]
    last = None
    for user, passwd in creds:
        ftp = FTP()
        try:
            ftp.connect(HOST, 21, timeout=15)
            ftp.login(user or "anonymous", passwd)
            ftp.set_pasv(True)
            with LOCAL_INI.open("rb") as fh:
                ftp.storbinary("STOR /pdpconfig-211bsd.ini", fh)
            with LOCAL_INI.open("rb") as fh:
                ftp.storbinary("STOR /pdpconfig.ini", fh)
            print(f"FTP uploaded ini as {user!r}", flush=True)
            ftp.quit()
            return
        except Exception as exc:
            last = exc
            try:
                ftp.close()
            except Exception:
                pass
    raise RuntimeError(f"FTP failed: {last}")


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
        data = drain(conn, 0.7)
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
    ser.close()


def main() -> int:
    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    serial_log = OUT / f"{stamp}-clock-com18.log"
    telnet_log = OUT / f"{stamp}-clock-telnet.log"

    print("=== FTP fresh config ===", flush=True)
    ftp_upload()

    stop = threading.Event()
    thr = threading.Thread(target=serial_reader, args=(stop, serial_log), daemon=True)
    thr.start()

    conn = TelnetConnection(HOST, 23, timeout=2.0)
    for attempt in range(20):
        try:
            conn.connect()
            break
        except Exception as exc:
            print("telnet", attempt, exc, flush=True)
            time.sleep(2)
            conn = TelnetConnection(HOST, 23, timeout=2.0)
    else:
        raise RuntimeError("telnet down")

    ensure_shell(conn)
    profile = BootProfile(
        name="211bsd",
        config_path="/pdpconfig-211bsd.ini",
        completion=b"login: ",
        quiet_seconds=2.0,
    )
    install_config(conn, profile, 12.0, True)
    shell_command(conn, "set pcping=0", 1.0, True)
    shell_command(conn, "set", 2.0, True)

    conn.send(b"reset\r")
    time.sleep(0.2)
    conn.send(b"exit\r")
    drain(conn, 1.0)

    buf = bytearray()
    start = time.monotonic()
    last_out = start
    last_cr = 0.0
    cr_n = 0
    phys_at = None
    armed = False
    deadline = start + 220.0

    while time.monotonic() < deadline:
        now = time.monotonic()
        if cr_n < 20 and now - start < 45 and b"2.11 BSD" not in buf and now - last_cr >= 2.0:
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
            if b"phys mem" in buf and not armed:
                print("\n*** phys mem — arm clock_trace ***\n", flush=True)
                ensure_shell(conn)
                sh(conn, "set pcping=0", 1.0)
                sh(conn, "set clock_trace=200", 1.0)
                sh(conn, "exit", 0.5)
                armed = True
                phys_at = time.monotonic()
                last_out = phys_at
            if b"login: " in buf or b"configure system" in buf:
                break
            continue
        if phys_at and now - last_out > 30:
            print("\n*** quiet — dump ***\n", flush=True)
            break

    telnet_log.write_bytes(buf)
    time.sleep(1.0)

    ensure_shell(conn)
    print("\n=== hang dump ===\n", flush=True)
    for cmd in ("rl regs", "tty", "lights", "set"):
        print(f">>> {cmd}", flush=True)
        sh(conn, cmd, 2.0)

    conn.send(b"monitor\r")
    drain(conn, 0.4)
    for cmd, w in (
        ("P", 1.5),
        ("MI026500", 1.0),
        ("MI026600", 1.0),
        ("MI025500", 1.0),
        ("D077122", 1.0),
        ("D177546", 1.0),  # physical RAM only — may be useless
        ("U", 2.5),
        ("C", 0.3),
        (">", 0.3),
    ):
        print(f">>> mon {cmd}", flush=True)
        conn.send(cmd.encode() + b"\r")
        drain(conn, w)

    sh(conn, "set clock_trace=0", 1.0)
    sh(conn, "exit", 0.5)
    stop.set()
    thr.join(timeout=2)
    conn.close()

    text = serial_log.read_text(encoding="latin-1", errors="replace")
    keys = ("KW11", "LKS", "177546", "clock", "IRQ", "tick", "BR6", "0100")
    print("\n=== COM18 clock-related lines (sample) ===\n", flush=True)
    lines = [ln for ln in text.splitlines() if any(k.lower() in ln.lower() for k in keys)]
    for ln in lines[-40:]:
        print(ln, flush=True)
    print(f"\nmatched {len(lines)} / {len(text.splitlines())} lines", flush=True)
    print(f"telnet={telnet_log}\ncom18={serial_log}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
