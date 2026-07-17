#!/usr/bin/env python3
"""One-shot XXDP diagnostic runner over the vpdp1170 USB serial console.

This is the checked-in version of the small capture scripts used during RP/RH70
diagnostic work. It owns COM18 directly, resets the ESP32-S3 using the tested
DTR/RTS sequence, waits for the XXDP V2.5 prompt, sends an XXDP command, and
captures output until a stop marker, idle timeout, or hard timeout.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys
import time

try:
    import serial
except ImportError as exc:  # pragma: no cover - environment check
    raise SystemExit("pyserial is required: python -m pip install pyserial") from exc


XXDP_BANNER = "XXDP-XM EXTENDED MONITOR - XXDP V2.5"
DEFAULT_STOP_MARKERS = [
    "DR>",
    "HIT STOP ON DRIVE",
    "PROGRAM WILL HANG",
    "HALT instruction",
    "HALT regs",
    "?MON-F-",
    "END PASS",
    "PROGRAM MODE",
    "PROGRAM INITIALIZATION COMPLETE",
]


def parse_prompt_reply(value: str) -> tuple[str, str]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("prompt replies must use PROMPT=REPLY")
    prompt, reply = value.split("=", 1)
    if not prompt:
        raise argparse.ArgumentTypeError("prompt text must not be empty")
    return prompt.upper(), reply.replace("\\r", "\r").replace("\\n", "\n")


def append_output(path: Path | None, text: str) -> None:
    if not path:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", errors="replace")


def wait_for_xxdp_and_run(args: argparse.Namespace) -> str:
    command = args.command
    if not command.endswith("\r"):
        command += "\r"

    replies: list[tuple[str, str]] = list(args.reply or [])
    sent_replies: set[str] = set()
    stop_markers = list(DEFAULT_STOP_MARKERS)
    stop_markers.extend(args.stop_marker or [])

    text = ""
    sent_command = False
    command_start = 0
    last_output = time.monotonic()
    deadline = time.monotonic() + args.timeout_seconds

    with serial.Serial(args.port, args.baud, timeout=0.05, write_timeout=2.0) as ser:
        ser.reset_input_buffer()
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(args.reset_pulse_seconds)
        ser.setDTR(False)
        ser.setRTS(False)

        while time.monotonic() < deadline:
            data = ser.read(4096)
            if data:
                text += data.decode("utf-8", errors="replace")
                last_output = time.monotonic()

            if not sent_command and XXDP_BANNER in text and text.rstrip().endswith("."):
                ser.write(command.encode("ascii"))
                ser.flush()
                sent_command = True
                command_start = len(text)
                text += command.replace("\r", "\r\n")
                last_output = time.monotonic()

            if sent_command:
                after_command = text[command_start:]
                upper_after = after_command.upper()
                for prompt, reply in replies:
                    if prompt in upper_after and prompt not in sent_replies:
                        time.sleep(args.reply_delay_seconds)
                        ser.write(reply.encode("ascii"))
                        ser.flush()
                        text += reply.replace("\r", "\r\n")
                        sent_replies.add(prompt)
                        last_output = time.monotonic()
                        break

                if any(marker in after_command for marker in stop_markers):
                    break
                if args.max_unexpected_attention and (
                    after_command.count("UNEXPECTED ATTENTION OCCURRED") >= args.max_unexpected_attention
                    or after_command.count("RH11 INTERRUPT OCCURRED (RPAS=0)") >= args.max_unexpected_attention
                ):
                    break
                if time.monotonic() - last_output > args.idle_seconds:
                    break

            time.sleep(0.05)

    return text


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Reset vpdp1170, boot XXDP, run one diagnostic, and capture serial output.")
    parser.add_argument("command", help='XXDP command, for example "R ZRJDE0"')
    parser.add_argument("--port", default="COM18")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout-seconds", type=float, default=900.0)
    parser.add_argument("--idle-seconds", type=float, default=120.0)
    parser.add_argument("--reset-pulse-seconds", type=float, default=0.5)
    parser.add_argument("--reply-delay-seconds", type=float, default=0.2)
    parser.add_argument("--reply", action="append", type=parse_prompt_reply, help='Prompt automation, e.g. "ENTER DATE:=17-JUL-76\\r"')
    parser.add_argument("--stop-marker", action="append", help="Additional output marker that stops capture")
    parser.add_argument("--max-unexpected-attention", type=int, default=3)
    parser.add_argument("--output", type=Path, help="Optional capture file")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    text = wait_for_xxdp_and_run(args)
    append_output(args.output, text)
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
