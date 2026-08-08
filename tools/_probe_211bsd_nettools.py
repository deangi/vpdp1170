#!/usr/bin/env python3
"""Probe 2.11BSD guest over Telnet for networking tools /usr layout."""

from __future__ import annotations

import socket
import sys
import time

HOST = "192.168.7.144"
PORT = 23

CMDS = [
    "echo PATH=$PATH",
    "pwd",
    "df",
    "mount",
    "ls -l /usr",
    "ls /usr/ucb",
    "ls /usr/bin",
    "ls /etc",
    "ls /sbin",
    "ls /bin",
    "ls -l /usr/ucb/telnet /usr/ucb/ftp /usr/ucb/rlogin /usr/ucb/rcp 2>&1",
    "ls -l /etc/route /sbin/route /bin/route /usr/bin/route 2>&1",
    "ls /usr/src/ucb 2>&1 | head",
    "cat /etc/fstab",
    "disklabel rl0 2>&1 | tail -20",
    "disklabel rl1 2>&1 | tail -20",
]


def recv_for(sock: socket.socket, seconds: float) -> bytes:
    end = time.monotonic() + seconds
    out = bytearray()
    sock.settimeout(0.3)
    while time.monotonic() < end:
        try:
            chunk = sock.recv(4096)
            if not chunk:
                break
            out.extend(chunk)
        except socket.timeout:
            continue
    return bytes(out)


def main() -> int:
    s = socket.create_connection((HOST, PORT), timeout=8)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print(f"connected {HOST}:{PORT}", flush=True)

    # Negotiating telnet: ignore IAC for a bit, then wake the console.
    boot = recv_for(s, 1.5)
    if boot:
        sys.stdout.write(boot.decode("latin-1", errors="replace"))
        sys.stdout.flush()

    # Escape any management shell if somehow attached; prefer guest console.
    # Send CR to refresh prompt.
    s.sendall(b"\r")
    time.sleep(0.4)
    prompt = recv_for(s, 1.0)
    if prompt:
        sys.stdout.write(prompt.decode("latin-1", errors="replace"))
        sys.stdout.flush()

    transcript = bytearray()
    for cmd in CMDS:
        print(f"\n##### {cmd}", flush=True)
        s.sendall(cmd.encode("ascii") + b"\r")
        chunk = recv_for(s, 2.5)
        transcript.extend(chunk)
        sys.stdout.write(chunk.decode("latin-1", errors="replace"))
        sys.stdout.flush()

    s.close()
    out = "tools/_probe_211bsd_nettools.out"
    with open(out, "wb") as f:
        f.write(transcript)
    print(f"\n\nWrote {out}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
