#!/usr/bin/env python3
"""Measure vpdp1170 guest operating-system boot times over Telnet.

The runner enters the vpdp1170 management shell, replaces /pdpconfig.ini
with a selected /pdpconfig-<profile>.ini, then sends `reset` and `exit` as
one Telnet write.  Sending both commands together ensures the management
shell returns the connection to the guest before the cold boot begins.

Results are appended to CSV and the complete console transcript for every
run is saved alongside it.
"""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
import queue
import re
import socket
import sys
import threading
import time
from typing import Iterable


IAC = 255
DONT = 254
DO = 253
WONT = 252
WILL = 251
SB = 250
SE = 240

OPT_BINARY = 0
OPT_ECHO = 1
OPT_SGA = 3

SHELL_PROMPT_RE = re.compile(rb"vpdp:[^\r\n>]{0,120}> ")
SHELL_BANNER = b"vpdp1170 management shell"


@dataclass(frozen=True)
class BootAction:
    """Input to send after a boot-time question is observed."""

    prompt: bytes
    response: bytes
    occurrence: int = 1
    description: str = ""


@dataclass(frozen=True)
class BootProfile:
    name: str
    config_path: str
    completion_kind: str = "fixed"
    completion: bytes | None = None
    occurrence: int = 1
    quiet_seconds: float = 1.0
    actions: tuple[BootAction, ...] = field(default_factory=tuple)
    notes: str = ""


PROFILE_ORDER = (
    "rsx11mp46",
    "rt11v5",
    "unix6",
    "11mark",
    "rstsv4",
    "rsx11m",
    "rsx11mp46-pidp",
    "xxdp25",
)

PROFILES = {
    "rsx11mp46": BootProfile(
        name="rsx11mp46",
        config_path="/pdpconfig-rsx11mp46.ini",
        completion=b">@DL:[1,2]STARTUP",
        quiet_seconds=1.0,
    ),
    "rt11v5": BootProfile(
        name="rt11v5",
        config_path="/pdpconfig-rt11v5.ini",
        completion_kind="dot_prompt",
        quiet_seconds=1.0,
        notes="A line containing only '.' is treated as the monitor prompt.",
    ),
    "unix6": BootProfile(
        name="unix6",
        config_path="/pdpconfig-unixv6.ini",
        completion=b"login: ",
        quiet_seconds=1.0,
    ),
    "11mark": BootProfile(
        name="11mark",
        config_path="/pdpconfig-11mark.ini",
        completion=b"Please enter time and date (HH:MM DD-MMM-YY) [S]:",
        quiet_seconds=1.0,
    ),
    "rstsv4": BootProfile(
        name="rstsv4",
        config_path="/pdpconfig-rstsv4.ini",
        completion=b"Ready\r\n",
        occurrence=2,
        quiet_seconds=1.0,
    ),
    "rsx11m": BootProfile(
        name="rsx11m",
        config_path="/pdpconfig-rsx11m.ini",
        completion=b"PLEASE ENTER TIME AND DATE (HR:MN DD-MMM-YY) [S]:",
        quiet_seconds=1.0,
    ),
    "rsx11mp46-pidp": BootProfile(
        name="rsx11mp46-pidp",
        config_path="/pdpconfig-rsx11mp46-pidp.ini",
        completion=(
            b"Please enter time and date (HH:MM DD-MMM-YYYY) [S T:1M]:"
        ),
        quiet_seconds=1.0,
    ),
    "xxdp25": BootProfile(
        name="xxdp25",
        config_path="/pdpconfig-xxdp25.ini",
        completion_kind="dot_prompt",
        quiet_seconds=1.0,
        notes="A line containing only '.' is treated as the monitor prompt.",
    ),
}


class BenchmarkError(RuntimeError):
    pass


class BootRunError(BenchmarkError):
    def __init__(self, message: str, transcript: bytes):
        super().__init__(message)
        self.transcript = transcript


class SerialCapture:
    """Own a serial port continuously for the complete benchmark suite."""

    def __init__(self, port: str, baud: int, path: Path):
        self.port = port
        self.baud = baud
        self.path = path
        self._serial = None
        self._handle = None
        self._stop = threading.Event()
        self._reader_thread: threading.Thread | None = None
        self._writer_thread: threading.Thread | None = None
        self._write_queue: queue.SimpleQueue[bytes | None] = queue.SimpleQueue()
        self.error = ""

    def start(self) -> None:
        try:
            import serial
        except ImportError as exc:
            raise BenchmarkError(
                "PySerial is required when --serial-port is used"
            ) from exc

        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._handle = self.path.open("wb")
        header = (
            f"vpdp1170 boot benchmark serial capture\r\n"
            f"started={datetime.now().astimezone().isoformat()}\r\n"
            f"port={self.port} baud={self.baud}\r\n"
            f"--- serial stream ---\r\n"
        )
        self._handle.write(header.encode("utf-8"))
        self._handle.flush()

        try:
            device = serial.Serial()
            device.port = self.port
            device.baudrate = self.baud
            device.timeout = 0.10
            device.dtr = False
            device.rts = False
            device.open()
            self._serial = device
        except Exception as exc:
            self._handle.close()
            self._handle = None
            raise BenchmarkError(
                f"cannot open serial port {self.port}: {exc}"
            ) from exc

        self._stop.clear()
        self._writer_thread = threading.Thread(
            target=self._writer, name="serial-log-writer", daemon=True
        )
        self._reader_thread = threading.Thread(
            target=self._reader, name="serial-capture", daemon=True
        )
        self._writer_thread.start()
        self._reader_thread.start()

    def _reader(self) -> None:
        while not self._stop.is_set():
            try:
                waiting = self._serial.in_waiting if self._serial else 0
                data = self._serial.read(max(1, min(waiting, 4096)))
            except Exception as exc:
                self.error = str(exc)
                return
            if data:
                # Never perform filesystem I/O on the serial-reader thread.
                # A slow OneDrive flush must not stop COM18 draining and apply
                # guest console backpressure.
                self._write_queue.put(data)

    def _writer(self) -> None:
        last_flush = time.monotonic()
        while True:
            try:
                data = self._write_queue.get(timeout=0.25)
            except queue.Empty:
                data = b""
            if data is None:
                break
            if data and self._handle:
                self._handle.write(data)
            now = time.monotonic()
            if self._handle and now - last_flush >= 1.0:
                self._handle.flush()
                last_flush = now

    def stop(self) -> None:
        self._stop.set()
        if self._reader_thread:
            self._reader_thread.join(timeout=2.0)
        self._write_queue.put(None)
        if self._writer_thread:
            self._writer_thread.join(timeout=30.0)
        if self._serial:
            try:
                self._serial.close()
            except Exception:
                pass
        self._serial = None
        if self._handle:
            self._handle.flush()
            self._handle.close()
        self._handle = None


class TelnetConnection:
    """Small RFC-854 client sufficient for the vpdp1170 console."""

    def __init__(self, host: str, port: int, timeout: float):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.sock: socket.socket | None = None
        self._state = "data"
        self._iac_command = 0

    def connect(self) -> None:
        self.close()
        self.sock = socket.create_connection(
            (self.host, self.port), timeout=self.timeout
        )
        self.sock.settimeout(0.10)
        self._state = "data"
        self._iac_command = 0

    def close(self) -> None:
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = None

    def send(self, data: bytes) -> None:
        if not self.sock:
            raise BenchmarkError("Telnet is not connected")
        self.sock.sendall(data)

    def _reply_to_negotiation(self, command: int, option: int) -> None:
        if not self.sock:
            return
        if command == WILL:
            reply = DO if option in (OPT_BINARY, OPT_ECHO, OPT_SGA) else DONT
        elif command == WONT:
            reply = DONT
        elif command == DO:
            reply = WILL if option in (OPT_BINARY, OPT_SGA) else WONT
        else:
            reply = WONT
        self.sock.sendall(bytes((IAC, reply, option)))

    def _decode_telnet(self, incoming: bytes) -> bytes:
        output = bytearray()
        for value in incoming:
            if self._state == "data":
                if value == IAC:
                    self._state = "iac"
                else:
                    output.append(value)
            elif self._state == "iac":
                if value == IAC:
                    output.append(IAC)
                    self._state = "data"
                elif value in (WILL, WONT, DO, DONT):
                    self._iac_command = value
                    self._state = "option"
                elif value == SB:
                    self._state = "subneg"
                else:
                    self._state = "data"
            elif self._state == "option":
                self._reply_to_negotiation(self._iac_command, value)
                self._state = "data"
            elif self._state == "subneg":
                if value == IAC:
                    self._state = "subneg_iac"
            elif self._state == "subneg_iac":
                self._state = "data" if value == SE else "subneg"
        return bytes(output)

    def receive(self) -> bytes:
        if not self.sock:
            raise BenchmarkError("Telnet is not connected")
        try:
            incoming = self.sock.recv(4096)
        except socket.timeout:
            return b""
        except OSError as exc:
            raise BenchmarkError(f"Telnet receive failed: {exc}") from exc
        if not incoming:
            raise BenchmarkError("Telnet connection closed by the board")
        return self._decode_telnet(incoming)


def count_occurrences(data: bytes, pattern: bytes) -> int:
    count = 0
    start = 0
    while True:
        position = data.find(pattern, start)
        if position < 0:
            return count
        count += 1
        start = position + len(pattern)


def latest_occurrence_end(data: bytes, pattern: bytes) -> int:
    position = data.rfind(pattern)
    return -1 if position < 0 else position + len(pattern)


class CompletionDetector:
    def __init__(self, profile: BootProfile):
        self.profile = profile
        self.candidate_at: float | None = None
        self.candidate_position = -1

    def feed(self, transcript: bytes, received_at: float) -> None:
        if self.profile.completion_kind == "dot_prompt":
            stripped = transcript.rstrip(b"\x00\r\n\t ")
            lines = re.split(rb"[\r\n]+", stripped)
            is_prompt = bool(lines) and lines[-1].strip() == b"."
            position = len(stripped) if is_prompt else -1
        else:
            pattern = self.profile.completion
            if not pattern or count_occurrences(transcript, pattern) < self.profile.occurrence:
                position = -1
            else:
                match_end = latest_occurrence_end(transcript, pattern)
                suffix = transcript[match_end:]
                # A terminal may append CR/LF after the visible prompt. Other
                # output invalidates this candidate until the prompt repeats.
                position = len(transcript) if not suffix.strip(b"\x00\r\n\t ") else -1

        if position < 0:
            self.candidate_at = None
            self.candidate_position = -1
        elif position != self.candidate_position:
            self.candidate_at = received_at
            self.candidate_position = position

    def complete(self, now: float, last_output_at: float) -> bool:
        return (
            self.candidate_at is not None
            and now - last_output_at >= self.profile.quiet_seconds
        )


def wait_for_data(
    connection: TelnetConnection,
    predicate,
    timeout: float,
    description: str,
    show_output: bool,
) -> bytes:
    deadline = time.monotonic() + timeout
    data = bytearray()
    while time.monotonic() < deadline:
        chunk = connection.receive()
        if not chunk:
            continue
        data.extend(chunk)
        if show_output:
            sys.stdout.write(chunk.decode("latin-1", errors="replace"))
            sys.stdout.flush()
        if b"console already in use" in data:
            raise BenchmarkError("the board reports that Telnet is already in use")
        if predicate(bytes(data)):
            return bytes(data)
    raise BenchmarkError(f"timed out waiting for {description}")


def enter_shell(
    connection: TelnetConnection, shell_timeout: float, show_output: bool
) -> None:
    # Drain initial Telnet negotiation and any guest output already in flight.
    settle_deadline = time.monotonic() + 0.25
    while time.monotonic() < settle_deadline:
        connection.receive()
    # A failed prior run may have left this Telnet client in management-shell
    # mode. Submit the current line first; an existing shell will answer with
    # its prompt, while a guest merely receives an innocuous carriage return.
    connection.send(b"\r")
    try:
        wait_for_data(
            connection,
            lambda data: SHELL_PROMPT_RE.search(data) is not None,
            min(2.0, shell_timeout),
            "an existing management-shell prompt",
            show_output,
        )
        return
    except BenchmarkError:
        pass

    connection.send(b"\x1b>>")
    wait_for_data(
        connection,
        lambda data: SHELL_BANNER in data and SHELL_PROMPT_RE.search(data),
        shell_timeout,
        "the management-shell prompt",
        show_output,
    )


def shell_command(
    connection: TelnetConnection,
    command: str,
    timeout: float,
    show_output: bool,
) -> bytes:
    connection.send(command.encode("ascii") + b"\r")
    return wait_for_data(
        connection,
        lambda data: SHELL_PROMPT_RE.search(data) is not None,
        timeout,
        f"completion of shell command {command!r}",
        show_output,
    )


def install_config(
    connection: TelnetConnection,
    profile: BootProfile,
    command_timeout: float,
    show_output: bool,
) -> None:
    # cp intentionally refuses to overwrite. Removing the active file is safe:
    # the emulator has already loaded it, and reset is not issued unless the
    # replacement copy is positively acknowledged.
    shell_command(
        connection,
        "rm /pdpconfig.ini",
        command_timeout,
        show_output,
    )
    response = shell_command(
        connection,
        f"cp {profile.config_path} /pdpconfig.ini",
        command_timeout,
        show_output,
    )
    expected = (
        f"copied {profile.config_path} -> /pdpconfig.ini".encode("ascii")
    )
    if expected not in response:
        text = response.decode("latin-1", errors="replace").strip()
        raise BenchmarkError(
            f"configuration copy was not acknowledged for {profile.name}: {text}"
        )


def write_transcript(
    output_dir: Path,
    profile: BootProfile,
    run_number: int,
    transcript: bytes,
    started_wall: datetime,
    status: str,
    error: str,
) -> Path:
    stamp = started_wall.strftime("%Y%m%d-%H%M%S")
    path = output_dir / f"{stamp}-{profile.name}-run{run_number:02d}.log"
    header = (
        f"profile={profile.name}\n"
        f"config={profile.config_path}\n"
        f"started={started_wall.isoformat()}\n"
        f"status={status}\n"
        f"error={error}\n"
        "--- console transcript ---\n"
    ).encode("utf-8")
    path.write_bytes(header + transcript)
    return path


def append_result(csv_path: Path, result: dict[str, object]) -> None:
    fieldnames = (
        "started",
        "profile",
        "run",
        "config",
        "status",
        "boot_seconds",
        "observed_seconds",
        "quiet_seconds",
        "bytes_received",
        "error",
        "transcript",
    )
    write_header = not csv_path.exists() or csv_path.stat().st_size == 0
    with csv_path.open("a", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        if write_header:
            writer.writeheader()
        writer.writerow(result)
        handle.flush()


def run_boot(
    connection: TelnetConnection,
    profile: BootProfile,
    timeout: float,
    show_output: bool,
) -> tuple[bytes, float, float]:
    detector = CompletionDetector(profile)
    transcript = bytearray()
    action_done = [False] * len(profile.actions)

    # The shell processes both queued commands before loop() consumes the
    # reboot request. This returns Telnet to the PDP console before cold boot.
    connection.send(b"reset\rexit\r")
    started = time.monotonic()
    deadline = started + timeout
    last_output_at = started

    while time.monotonic() < deadline:
        try:
            chunk = connection.receive()
        except BenchmarkError as exc:
            raise BootRunError(str(exc), bytes(transcript)) from exc
        now = time.monotonic()
        if chunk:
            transcript.extend(chunk)
            last_output_at = now
            if show_output:
                sys.stdout.write(chunk.decode("latin-1", errors="replace"))
                sys.stdout.flush()

            data = bytes(transcript)
            if b"error: emulator command queue full" in data:
                raise BootRunError(
                    "the emulator rejected the reset command", bytes(transcript)
                )

            for index, action in enumerate(profile.actions):
                if action_done[index]:
                    continue
                if count_occurrences(data, action.prompt) >= action.occurrence:
                    connection.send(action.response)
                    action_done[index] = True
                    label = action.description or action.prompt.decode(
                        "latin-1", errors="replace"
                    )
                    print(f"\n  answered boot prompt: {label}", flush=True)

            detector.feed(data, now)

        if detector.complete(now, last_output_at):
            observed = now - started
            assert detector.candidate_at is not None
            boot_seconds = detector.candidate_at - started
            return bytes(transcript), boot_seconds, observed

    raise BootRunError(
        f"boot did not reach the {profile.name} completion prompt "
        f"within {timeout:.0f} seconds",
        bytes(transcript),
    )


def select_profiles(names: Iterable[str]) -> list[BootProfile]:
    selected: list[str] = []
    for name in names:
        if name == "all":
            for profile_name in PROFILE_ORDER:
                if profile_name not in selected:
                    selected.append(profile_name)
        elif name not in selected:
            selected.append(name)
    return [PROFILES[name] for name in selected]


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Benchmark vpdp1170 guest operating-system boot times"
    )
    parser.add_argument("--host", required=True, help="vpdp1170 IP address or host name")
    parser.add_argument("--port", type=int, default=23, help="Telnet port (default: 23)")
    parser.add_argument(
        "--os",
        nargs="+",
        choices=("all",) + PROFILE_ORDER,
        default=["all"],
        help="profiles to run, in the specified order (default: all)",
    )
    parser.add_argument("--runs", type=int, default=1, help="runs per profile")
    parser.add_argument(
        "--timeout",
        type=float,
        default=600.0,
        help="maximum seconds for each guest boot (default: 600)",
    )
    parser.add_argument(
        "--shell-timeout",
        type=float,
        default=15.0,
        help="seconds to wait for shell operations (default: 15)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("boot-benchmark-results"),
        help="results directory",
    )
    parser.add_argument(
        "--serial-port",
        help="hold this serial port open and capture it for the entire suite",
    )
    parser.add_argument(
        "--serial-baud",
        type=int,
        default=115200,
        help="serial capture baud rate (default: 115200)",
    )
    parser.add_argument(
        "--serial-log",
        type=Path,
        help="serial log path (default: results directory with timestamp)",
    )
    parser.add_argument(
        "--serial-settle",
        type=float,
        default=15.0,
        help="seconds to wait after opening serial before Telnet (default: 15)",
    )
    parser.add_argument(
        "--show-output",
        action="store_true",
        help="mirror guest and management-shell output to the terminal",
    )
    parser.add_argument(
        "--continue-on-error",
        action="store_true",
        help="continue with later profiles after a failed run",
    )
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.runs < 1:
        raise SystemExit("--runs must be at least 1")
    if args.timeout <= 0 or args.shell_timeout <= 0:
        raise SystemExit("timeouts must be positive")

    args.output.mkdir(parents=True, exist_ok=True)
    csv_path = args.output / "boot-times.csv"
    profiles = select_profiles(args.os)
    connection = TelnetConnection(args.host, args.port, args.shell_timeout)
    serial_capture: SerialCapture | None = None
    failures = 0

    try:
        if args.serial_port:
            serial_path = args.serial_log or (
                args.output
                / f"suite-serial-{datetime.now().strftime('%Y%m%d-%H%M%S')}.log"
            )
            serial_capture = SerialCapture(
                args.serial_port, args.serial_baud, serial_path
            )
            serial_capture.start()
            print(f"Serial capture: {serial_path}", flush=True)
            # Opening the ESP32-S3 serial endpoint can reboot the board.
            # Keep the handle open and allow WiFi/Telnet to return.
            if args.serial_settle > 0:
                time.sleep(args.serial_settle)

        connection.connect()
        for profile in profiles:
            if profile.completion is None and profile.completion_kind != "dot_prompt":
                print(
                    f"SKIP {profile.name}: {profile.notes or 'no completion detector'}",
                    flush=True,
                )
                continue

            for run_number in range(1, args.runs + 1):
                started_wall = datetime.now().astimezone()
                transcript = b""
                boot_seconds: float | str = ""
                observed_seconds: float | str = ""
                status = "failed"
                error = ""
                print(
                    f"[{profile.name} run {run_number}/{args.runs}] "
                    f"installing {profile.config_path}",
                    flush=True,
                )
                try:
                    enter_shell(connection, args.shell_timeout, args.show_output)
                    install_config(
                        connection,
                        profile,
                        args.shell_timeout,
                        args.show_output,
                    )
                    print(
                        f"[{profile.name} run {run_number}/{args.runs}] booting",
                        flush=True,
                    )
                    transcript, measured, observed = run_boot(
                        connection,
                        profile,
                        args.timeout,
                        args.show_output,
                    )
                    boot_seconds = f"{measured:.3f}"
                    observed_seconds = f"{observed:.3f}"
                    status = "passed"
                    print(
                        f"[{profile.name} run {run_number}/{args.runs}] "
                        f"PASS boot={measured:.3f}s "
                        f"(confirmed after {observed:.3f}s)",
                        flush=True,
                    )
                except (BenchmarkError, OSError) as exc:
                    failures += 1
                    if isinstance(exc, BootRunError):
                        transcript = exc.transcript
                    error = str(exc)
                    print(
                        f"[{profile.name} run {run_number}/{args.runs}] "
                        f"FAIL: {error}",
                        file=sys.stderr,
                        flush=True,
                    )

                transcript_path = write_transcript(
                    args.output,
                    profile,
                    run_number,
                    transcript,
                    started_wall,
                    status,
                    error,
                )
                append_result(
                    csv_path,
                    {
                        "started": started_wall.isoformat(),
                        "profile": profile.name,
                        "run": run_number,
                        "config": profile.config_path,
                        "status": status,
                        "boot_seconds": boot_seconds,
                        "observed_seconds": observed_seconds,
                        "quiet_seconds": profile.quiet_seconds,
                        "bytes_received": len(transcript),
                        "error": error,
                        "transcript": str(transcript_path),
                    },
                )

                if status != "passed" and not args.continue_on_error:
                    return 1
    except (BenchmarkError, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("\nInterrupted; completed results remain in the CSV.", file=sys.stderr)
        return 130
    finally:
        connection.close()
        if serial_capture:
            serial_capture.stop()
            if serial_capture.error:
                print(
                    f"WARNING: serial capture ended with: {serial_capture.error}",
                    file=sys.stderr,
                )

    print(f"Results: {csv_path}", flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
