#!/usr/bin/env python3
"""Try xp/rl usr mounts and inventory block devices."""

from __future__ import annotations

import socket
import sys
import time

HOST = "192.168.7.144"
PORT = 23
IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240

CMDS = [
    "ls -l /dev/xp0* /dev/rl0* /dev/rl1* 2>&1",
    "ls -l /dev | grep -E '^[bc]' | head -40",
    "disklabel xp0",
    "mount /dev/xp0c /usr 2>&1",
    "ls /usr 2>&1",
    "umount /usr 2>&1",
    "mount /dev/rl1c /usr 2>&1",
    "ls /usr 2>&1",
    "umount /usr 2>&1",
    "mount /dev/rl1a /usr 2>&1",
    "ls /usr 2>&1",
    "umount /usr 2>&1",
    "mount /dev/rl1g /usr 2>&1",
    "ls /usr 2>&1",
    "ls /usr/ucb 2>&1",
    "umount /usr 2>&1",
    "file /vmunix 2>&1",
    "strings /vmunix | grep -E 'rl0|ra0|xp0|uda|hk|hp' | head -20",
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

    def cmd(self, line: str, timeout: float = 12.0) -> None:
        print(f"\n===== {line} =====", flush=True)
        self.sock.sendall(line.encode("ascii") + b"\r")
        self.recv_until(b"\n# ", timeout)


def main() -> int:
    t = Telnet()
    print(f"connected {HOST}:{PORT}", flush=True)
    t.sock.sendall(b"\r")
    t.recv_until(b"# ", 5.0)
    for c in CMDS:
        t.cmd(c, 20.0 if "strings" in c or "disklabel" in c else 10.0)
    t.sock.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
