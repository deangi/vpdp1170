"""Small JSON-lines command server used by toolkit services."""

from __future__ import annotations

from collections.abc import Callable
import json
import socketserver
import threading
from typing import Any


CommandHandler = Callable[[dict[str, Any]], dict[str, Any]]


class JsonLineHandler(socketserver.StreamRequestHandler):
    def handle(self) -> None:
        server = self.server
        assert isinstance(server, JsonLineServer)

        for raw in self.rfile:
            try:
                request = json.loads(raw.decode("utf-8"))
                if not isinstance(request, dict):
                    raise ValueError("request must be a JSON object")
                response = server.command_handler(request)
            except Exception as exc:  # Keep service alive after bad commands.
                response = {"ok": False, "error": str(exc)}

            self.wfile.write((json.dumps(response) + "\n").encode("utf-8"))
            self.wfile.flush()

            if response.get("exit_service"):
                threading.Thread(target=server.shutdown, daemon=True).start()
                return


class JsonLineServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True

    def __init__(self, address: tuple[str, int], command_handler: CommandHandler):
        super().__init__(address, JsonLineHandler)
        self.command_handler = command_handler


def serve_json_lines(host: str, port: int, command_handler: CommandHandler) -> None:
    with JsonLineServer((host, port), command_handler) as server:
        server.serve_forever()
