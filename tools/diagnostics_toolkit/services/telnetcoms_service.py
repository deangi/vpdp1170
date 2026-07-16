#!/usr/bin/env python3
"""TelnetComs service for vpdp1170 telnet control."""

from __future__ import annotations

import argparse
from pathlib import Path
import socket
import sys
import threading
import time
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from diagtoolkit.config import TelnetConfig, load_config
from diagtoolkit.line_server import serve_json_lines
from diagtoolkit.ring_buffer import TextRingBuffer


class TelnetComs:
    def __init__(self, config: TelnetConfig):
        self.config = config
        self.buffer = TextRingBuffer(config.read_buffer_limit)
        self._socket: socket.socket | None = None
        self._lock = threading.Lock()
        self._reader_stop = threading.Event()
        self._reader_thread: threading.Thread | None = None
        self._last_error = ""
        self._mode = "disconnected"

    def start(self) -> None:
        self.connect(required=False)

    def connect(self, required: bool = True) -> dict[str, Any]:
        with self._lock:
            if self._socket:
                return {"ok": True, "connected": True}
            try:
                sock = socket.create_connection(
                    (self.config.target_host, self.config.target_port),
                    timeout=10.0,
                )
            except OSError as exc:
                self._last_error = str(exc)
                self._mode = "disconnected"
                if required:
                    return {"ok": False, "connected": False, "error": str(exc)}
                return {"ok": True, "connected": False, "error": str(exc)}
            sock.settimeout(0.05)
            self._socket = sock
            self._mode = "pdp"
            self._reader_stop.clear()
            if not self._reader_thread or not self._reader_thread.is_alive():
                self._reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
                self._reader_thread.start()
        return {"ok": True, "connected": True, "mode": self._mode}

    def _reader_loop(self) -> None:
        while not self._reader_stop.is_set():
            sock = self._socket
            if not sock:
                time.sleep(0.1)
                continue
            try:
                data = sock.recv(4096)
                if data:
                    self.buffer.append(data.decode("utf-8", errors="replace"))
                else:
                    self._last_error = "telnet connection closed"
                    self._close_socket()
            except socket.timeout:
                continue
            except OSError as exc:
                self._last_error = str(exc)
                self._close_socket()
                time.sleep(0.25)

    def _close_socket(self) -> None:
        sock = self._socket
        self._socket = None
        self._mode = "disconnected"
        if sock:
            try:
                sock.close()
            except OSError:
                pass

    def stop(self) -> None:
        self._reader_stop.set()
        self._close_socket()
        if self._reader_thread:
            self._reader_thread.join(timeout=2.0)

    def status(self) -> dict[str, Any]:
        return {
            "ok": True,
            "service": "TelnetComs",
            "target_host": self.config.target_host,
            "target_port": self.config.target_port,
            "connected": self._socket is not None,
            "mode": self._mode,
            "buffer_size": self.buffer.size(),
            "last_error": self._last_error,
        }

    def send(self, text: str) -> dict[str, Any]:
        with self._lock:
            if not self._socket:
                return {"ok": False, "error": "telnet is not connected"}
            self._socket.sendall(text.encode("utf-8"))
        return {"ok": True, "sent_chars": len(text)}

    def transition(self, mode: str) -> dict[str, Any]:
        transitions = {
            "enter-shell": ("shell", "\x1b>>"),
            "enter-monitor": ("monitor", "monitor\r"),
            "exit-monitor": ("shell", ">\r"),
            "exit-shell": ("pdp", "exit\r"),
        }
        if mode not in transitions:
            return {"ok": False, "error": f"unknown transition: {mode}"}
        next_mode, text = transitions[mode]
        response = self.send(text)
        if response.get("ok"):
            self._mode = next_mode
            response["mode"] = self._mode
        return response

    def handle_command(self, request: dict[str, Any]) -> dict[str, Any]:
        command = str(request.get("command", "status"))

        if command == "status":
            return self.status()
        if command == "connect":
            return self.connect(required=bool(request.get("required", True)))
        if command == "disconnect":
            self._close_socket()
            return {"ok": True, "connected": False, "mode": self._mode}
        if command == "send":
            return self.send(str(request.get("text", "")))
        if command in ("enter-shell", "enter-monitor", "exit-monitor", "exit-shell"):
            return self.transition(command)
        if command == "read":
            return {
                "ok": True,
                "text": self.buffer.read(
                    clear=bool(request.get("clear", False)),
                    max_chars=int(request.get("max_chars", 0) or 0),
                ),
            }
        if command == "clear" or command == "flush":
            self.buffer.clear()
            return {"ok": True}
        if command == "exit":
            return {"ok": True, "exit_service": True}

        return {"ok": False, "error": f"unknown command: {command}"}


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="TelnetComs JSON-lines service")
    parser.add_argument("--config", type=Path, help="path to toolkit JSON config")
    parser.add_argument("--target-host", help="target board IP/host override")
    parser.add_argument("--target-port", type=int, help="target telnet port override")
    parser.add_argument("--control-host", help="control socket host override")
    parser.add_argument("--control-port", type=int, help="control socket port override")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    cfg = load_config(args.config).telnet
    cfg = TelnetConfig(
        target_host=args.target_host or cfg.target_host,
        target_port=args.target_port or cfg.target_port,
        control_host=args.control_host or cfg.control_host,
        control_port=args.control_port or cfg.control_port,
        read_buffer_limit=cfg.read_buffer_limit,
    )

    service = TelnetComs(cfg)
    service.start()
    try:
        print(
            "TelnetComs listening on "
            f"{cfg.control_host}:{cfg.control_port}, target={cfg.target_host}:{cfg.target_port}"
        )
        serve_json_lines(cfg.control_host, cfg.control_port, service.handle_command)
    finally:
        service.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
