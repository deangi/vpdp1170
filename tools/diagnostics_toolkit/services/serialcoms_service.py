#!/usr/bin/env python3
"""SerialComs service for vpdp1170 board control.

This process owns the serial port. Codex and helper scripts talk to it over a
localhost JSON-lines socket so the COM port is not repeatedly opened and closed.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys
import threading
import time
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

try:
    import serial
except ImportError as exc:  # pragma: no cover - depends on host install
    serial = None
    SERIAL_IMPORT_ERROR = exc
else:
    SERIAL_IMPORT_ERROR = None

from diagtoolkit.config import SerialConfig, load_config
from diagtoolkit.line_server import serve_json_lines
from diagtoolkit.ring_buffer import TextRingBuffer


class SerialComs:
    def __init__(self, config: SerialConfig):
        self.config = config
        self.buffer = TextRingBuffer(config.read_buffer_limit)
        self._serial: Any = None
        self._lock = threading.Lock()
        self._reader_stop = threading.Event()
        self._reader_thread: threading.Thread | None = None
        self._last_error = ""

    def start(self) -> None:
        if serial is None:
            raise SystemExit(
                "error: pyserial is required for SerialComs. Install with `python -m pip install pyserial`."
            ) from SERIAL_IMPORT_ERROR

        self.connect()
        self._reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self._reader_thread.start()

    def connect(self) -> dict[str, Any]:
        if serial is None:
            return {"ok": False, "error": "pyserial is not available"}
        with self._lock:
            if self._serial and self._serial.is_open:
                return {"ok": True, "connected": True}
            try:
                self._serial = serial.Serial(
                    self.config.port,
                    self.config.baud,
                    timeout=0.05,
                    write_timeout=2.0,
                )
                self._last_error = ""
            except Exception as exc:
                self._serial = None
                self._last_error = str(exc)
                return {"ok": False, "connected": False, "error": str(exc)}
        return {"ok": True, "connected": True}

    def disconnect(self) -> dict[str, Any]:
        with self._lock:
            if self._serial:
                try:
                    self._serial.close()
                finally:
                    self._serial = None
        return {"ok": True, "connected": False}

    def _reader_loop(self) -> None:
        while not self._reader_stop.is_set():
            serial_port = self._serial
            if not serial_port or not serial_port.is_open:
                time.sleep(0.1)
                continue
            try:
                data = serial_port.read(4096)
                if data:
                    self.buffer.append(data.decode("utf-8", errors="replace"))
            except Exception as exc:
                self._last_error = str(exc)
                time.sleep(0.25)

    def stop(self) -> None:
        self._reader_stop.set()
        if self._reader_thread:
            self._reader_thread.join(timeout=2.0)
        self.disconnect()

    def status(self) -> dict[str, Any]:
        is_open = bool(self._serial and self._serial.is_open)
        return {
            "ok": True,
            "service": "SerialComs",
            "port": self.config.port,
            "baud": self.config.baud,
            "connected": is_open,
            "buffer_size": self.buffer.size(),
            "last_error": self._last_error,
        }

    def send(self, text: str) -> dict[str, Any]:
        with self._lock:
            if not self._serial or not self._serial.is_open:
                return {"ok": False, "error": "serial port is not connected"}
            self._serial.write(text.encode("utf-8"))
            self._serial.flush()
        return {"ok": True, "sent_chars": len(text)}

    def reboot_board(self) -> dict[str, Any]:
        reset = self.config.reset
        if reset.method != "dtr_rts":
            return {"ok": False, "error": f"unsupported reset method: {reset.method}"}

        with self._lock:
            if not self._serial or not self._serial.is_open:
                return {"ok": False, "error": "serial port is not connected"}
            self._serial.dtr = False
            self._serial.rts = True
            time.sleep(reset.pulse_seconds)
            self._serial.dtr = False
            self._serial.rts = False
            time.sleep(reset.settle_seconds)

        return {"ok": True, "method": reset.method}

    def handle_command(self, request: dict[str, Any]) -> dict[str, Any]:
        command = str(request.get("command", "status"))

        if command == "status":
            return self.status()
        if command == "connect":
            return self.connect()
        if command == "disconnect":
            return self.disconnect()
        if command == "reboot-board":
            return self.reboot_board()
        if command == "send":
            return self.send(str(request.get("text", "")))
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
    parser = argparse.ArgumentParser(description="SerialComs JSON-lines service")
    parser.add_argument("--config", type=Path, help="path to toolkit JSON config")
    parser.add_argument("--port", help="serial port override, for example COM18")
    parser.add_argument("--baud", type=int, help="serial baud override")
    parser.add_argument("--control-host", help="control socket host override")
    parser.add_argument("--control-port", type=int, help="control socket port override")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    cfg = load_config(args.config).serial
    cfg = SerialConfig(
        port=args.port or cfg.port,
        baud=args.baud or cfg.baud,
        control_host=args.control_host or cfg.control_host,
        control_port=args.control_port or cfg.control_port,
        read_buffer_limit=cfg.read_buffer_limit,
        reset=cfg.reset,
    )

    service = SerialComs(cfg)
    service.start()
    try:
        print(f"SerialComs listening on {cfg.control_host}:{cfg.control_port}, serial={cfg.port}@{cfg.baud}")
        serve_json_lines(cfg.control_host, cfg.control_port, service.handle_command)
    finally:
        service.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
