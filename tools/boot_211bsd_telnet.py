#!/usr/bin/env python3
"""Telnet-only 2.11BSD boot: host answers the ':' prompt."""

from __future__ import annotations

import sys
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from benchmark_boot_times import (  # noqa: E402
    BootProfile,
    TelnetConnection,
    enter_shell,
    install_config,
    shell_command,
    BenchmarkError,
)

HOST = "192.168.7.144"
OUT = ROOT / "boot-benchmark-results-211bsd"
CAPTURE_SECS = 240.0


def main() -> int:
    OUT.mkdir(exist_ok=True)
    profile = BootProfile(
        name="211bsd",
        config_path="/pdpconfig-211bsd.ini",
        completion=b"login: ",
        quiet_seconds=2.0,
    )

    conn = TelnetConnection(HOST, 23, timeout=2.0)
    conn.connect()
    print("connected", flush=True)
    enter_shell(conn, 10.0, True)

    print("\n--- install ---", flush=True)
    install_config(conn, profile, 15.0, True)

    # Show whatever boot hooks the SD copy currently has.
    print("\n--- cat ---", flush=True)
    shell_command(conn, "cat /pdpconfig.ini", 15.0, True)

    print("\n--- reset+exit ---", flush=True)
    conn.send(b"reset\rexit\r")

    deadline = time.monotonic() + CAPTURE_SECS
    transcript = bytearray()
    last = time.monotonic()
    started = datetime.now()
    status = "timeout"
    prompts_answered = 0

    while time.monotonic() < deadline:
        try:
            chunk = conn.receive()
        except BenchmarkError as exc:
            print(f"\nTelnet error: {exc}", flush=True)
            status = "telnet_error"
            break

        if not chunk:
            if b"login: " in transcript and time.monotonic() - last >= 2.0:
                status = "ok"
                break
            continue

        transcript.extend(chunk)
        sys.stdout.write(chunk.decode("latin-1", errors="replace"))
        sys.stdout.flush()
        last = time.monotonic()
        data = bytes(transcript)

        # Answer idle ':' prompts from the host. Match with optional leading DEL.
        stripped = data.rstrip(b"\x00\r\n\t ")
        at_colon = (
            stripped.endswith(b":")
            or stripped.endswith(b"\x7f:")
        )
        if at_colon and prompts_answered < 3:
            # First try bare CR (distributed default). Later tries use explicit path.
            if prompts_answered == 0:
                reply = b"\r"
                label = "CR"
            else:
                reply = b"rl(0,0,0)unix\r"
                label = "rl(0,0,0)unix"
            time.sleep(0.6)  # match board boot_script delay
            conn.send(reply)
            prompts_answered += 1
            print(f"\n  host answered ':' with {label}", flush=True)

        if b"login: " in data and time.monotonic() - last >= 2.0:
            status = "ok"
            break
        if b"erase=" in data and b"# " in data and time.monotonic() - last >= 2.0:
            status = "single_user"
            break
        if (b"rlcs=" in data or b"err cy=" in data) and time.monotonic() - last >= 8.0:
            status = "rl_error"
            break
        if b"abort()" in data and time.monotonic() - last >= 2.0:
            status = "abort"
            break
        if b"Boot:" in data and b"bootdev=" in data:
            # Kernel load started — keep waiting.
            pass

    stamp = started.strftime("%Y%m%d-%H%M%S")
    path = OUT / f"{stamp}-211bsd-run01.log"
    header = (
        f"profile=211bsd\n"
        f"config=/pdpconfig-211bsd.ini\n"
        f"started={started.isoformat()}\n"
        f"status={status}\n"
        f"prompts_answered={prompts_answered}\n"
        "--- console transcript ---\n"
    ).encode("utf-8")
    path.write_bytes(header + bytes(transcript))
    print(f"\n\nSTATUS {status} bytes={len(transcript)} log={path}", flush=True)
    conn.close()
    return 0 if status in ("ok", "single_user") else 1


if __name__ == "__main__":
    raise SystemExit(main())
