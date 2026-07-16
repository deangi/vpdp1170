"""Client helper for JSON-lines control services."""

from __future__ import annotations

import json
import socket
from typing import Any


def send_command(host: str, port: int, command: dict[str, Any], timeout: float = 10.0) -> dict[str, Any]:
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.sendall((json.dumps(command) + "\n").encode("utf-8"))
        chunks: list[bytes] = []
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            chunks.append(chunk)
            if b"\n" in chunk:
                break

    data = b"".join(chunks).split(b"\n", 1)[0]
    if not data:
        return {"ok": False, "error": "empty response"}
    response = json.loads(data.decode("utf-8"))
    if not isinstance(response, dict):
        return {"ok": False, "error": "response was not a JSON object"}
    return response
