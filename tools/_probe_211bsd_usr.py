#!/usr/bin/env python3
"""Probe root device + try mounting a usr candidate; also list host disks."""

from __future__ import annotations

import socket
import sys
import time

HOST = "192.168.7.144"
PORT = 23
IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240

GUEST_CMDS = [
    "df",
    "ls -l /dev/root /dev/drum 2>&1",
    "sysctl kern.hostname 2>&1",
    "dmesg | grep -E 'ra0|rl0|xp0|root|de0' ",
    "disklabel ra0",
    "mount /dev/ra0g /usr 2>&1",
    "ls /usr 2>&1",
    "ls /usr/ucb 2>&1",
    "umount /usr 2>&1",
]


class Telnet:
    def __init__(self) -> None:
        self.sock = socket.create_connection((HOST, PORT), 8)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.settimeout(0.4)
        self._state = "data"
        self._cmd = 0

    def _nego(self, command: int, option: int) -> None:
        reply = DONT if command == WILL else WONT
        if command == DO:
            reply = WONT
        self.sock.sendall(bytes((IAC, reply, option)))

    def _decode(self, data: bytes) -> bytes:
        out = bytearray()
        for value in data:
            if self._state == "data":
                if value == IAC:
                    self._state = "iac"
                else:
                    out.append(value)
            elif self._state == "iac":
                if value == IAC:
                    out.append(IAC)
                    self._state = "data"
                elif value in (WILL, WONT, DO, DONT):
                    self._cmd = value
                    self._state = "option"
                elif value == SB:
                    self._state = "subneg"
                else:
                    self._state = "data"
            elif self._state == "option":
                self._nego(self._cmd, value)
                self._state = "data"
            elif self._state == "subneg":
                if value == IAC:
                    self._state = "subneg_iac"
            elif self._state == "subneg_iac":
                self._state = "data" if value == SE else "subneg"
        return bytes(out)

    def recv_until(self, needle: bytes, timeout: float) -> bytes:
        end = time.monotonic() + timeout
        buf = bytearray()
        while time.monotonic() < end:
            try:
                raw = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not raw:
                break
            chunk = self._decode(raw)
            if chunk:
                buf.extend(chunk)
                sys.stdout.write(chunk.decode("latin-1", errors="replace"))
                sys.stdout.flush()
                if needle in buf:
                    break
        return bytes(buf)

    def cmd(self, line: str, timeout: float = 12.0) -> bytes:
        print(f"\n===== {line} =====", flush=True)
        self.sock.sendall(line.encode("ascii") + b"\r")
        return self.recv_until(b"\n# ", timeout)


def shell_mode() -> None:
    """Enter management shell and show host drive mounts."""
    print("\n##### ENTER MGMT SHELL #####", flush=True)
    t = Telnet()
    t.sock.sendall(b"\r")
    t.recv_until(b"# ", 3.0)
    # ESC >> enters management shell on vpdp1170
    t.sock.sendall(b"\x1b>>")
    time.sleep(0.5)
    data = t.recv_until(b"vpdp:/>", 5.0)
    if b"vpdp:/>" not in data:
        # try again with CR
        t.sock.sendall(b"\r")
        t.recv_until(b"vpdp:/>", 3.0)
    for c in ("drives", "show", "exit"):
        print(f"\n===== shell:{c} =====", flush=True)
        t.sock.sendall(c.encode("ascii") + b"\r")
        needle = b"vpdp:/>" if c != "exit" else b"# "
        t.recv_until(needle, 8.0)
    t.sock.close()


def main() -> int:
    t = Telnet()
    print(f"connected {HOST}:{PORT}", flush=True)
    t.sock.sendall(b"\r")
    t.recv_until(b"# ", 5.0)
    for c in GUEST_CMDS:
        t.cmd(c, 15.0 if "dmesg" in c or "disklabel" in c or "mount" in c else 8.0)
    t.sock.close()
    time.sleep(0.5)
    shell_mode()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
