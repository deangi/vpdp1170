#!/usr/bin/env python3
"""Confirm xp0c /usr mount and list network clients."""

from __future__ import annotations

import socket
import sys
import time

HOST = "192.168.7.144"
PORT = 23
IAC, DONT, DO, WONT, WILL, SB, SE = 255, 254, 253, 252, 251, 250, 240

CMDS = [
    "mount /dev/xp0c /usr",
    "ls /usr",
    "ls /usr/ucb",
    "ls /usr/bin | grep tel",
    "ls /usr/ucb/telnet /usr/ucb/ftp /usr/ucb/rlogin 2>&1",
    "which telnet ftp ping",
    "echo PATH=$PATH",
    "df",
]


class Telnet:
    def __init__(self) -> None:
        self.sock = socket.create_connection((HOST, PORT), 8)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.settimeout(0.5)
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
                    # wait a bit more for trailing output after prompt-like #
                    time.sleep(0.3)
                    try:
                        while True:
                            more = self.sock.recv(4096)
                            if not more:
                                break
                            chunk2 = self._decode(more)
                            if chunk2:
                                buf.extend(chunk2)
                                sys.stdout.write(chunk2.decode("latin-1", errors="replace"))
                                sys.stdout.flush()
                    except socket.timeout:
                        pass
                    break
        return bytes(buf)

    def cmd(self, line: str, timeout: float = 15.0) -> None:
        print(f"\n===== {line} =====", flush=True)
        self.sock.sendall(line.encode("ascii") + b"\r")
        self.recv_until(b"\n# ", timeout)
        time.sleep(0.2)


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
