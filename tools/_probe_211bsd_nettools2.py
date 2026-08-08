#!/usr/bin/env python3
"""Cleaner 2.11BSD probe: wait for '# ' between commands."""

from __future__ import annotations

import socket
import sys
import time

HOST = "192.168.7.144"
PORT = 23
IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240

CMDS = [
    "/sbin/route",
    "ls /sbin | grep -i route",
    "ls /bin | grep -E 'ftp|telnet|rlogin|rsh'",
    "ls -la /usr",
    "cat /etc/fstab",
    "disklabel rl0",
    "disklabel rl1",
    "ls /dev/rl* /dev/ra* /dev/xp* 2>&1",
    "ls / | head -40",
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
        if command in (DO, DONT):
            reply = WONT if command == DO else DONT
        if command == WILL:
            reply = DONT
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

    def cmd(self, line: str, timeout: float = 8.0) -> bytes:
        print(f"\n===== {line} =====", flush=True)
        self.sock.sendall(line.encode("ascii") + b"\r")
        return self.recv_until(b"\n# ", timeout)


def main() -> int:
    t = Telnet()
    print(f"connected {HOST}:{PORT}", flush=True)
    t.sock.sendall(b"\r")
    t.recv_until(b"# ", 5.0)
    for c in CMDS:
        t.cmd(c)
    t.sock.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
