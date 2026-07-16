#!/usr/bin/env python3
"""Command client for Diagnostics Toolkit services."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

from diagtoolkit.client import send_command
from diagtoolkit.config import load_config


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Send one command to a diagnostics service")
    parser.add_argument("service", choices=("serial", "telnet"))
    parser.add_argument("command")
    parser.add_argument("--config", type=Path)
    parser.add_argument("--text", default="")
    parser.add_argument("--clear", action="store_true")
    parser.add_argument("--max-chars", type=int, default=0)
    parser.add_argument("--json", action="store_true", help="print full JSON response")
    return parser


def main() -> int:
    args = build_parser().parse_args()
    cfg = load_config(args.config)
    service_cfg = cfg.serial if args.service == "serial" else cfg.telnet
    command = {
        "command": args.command,
        "text": args.text,
        "clear": args.clear,
        "max_chars": args.max_chars,
    }
    response = send_command(service_cfg.control_host, service_cfg.control_port, command)

    if args.json:
        print(json.dumps(response, indent=2))
    elif "text" in response:
        print(response["text"], end="")
    else:
        print(json.dumps(response))

    return 0 if response.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main())
