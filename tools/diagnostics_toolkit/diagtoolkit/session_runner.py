"""Diagnostic session orchestration."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
import subprocess
import sys
import time
from typing import Any

from diagtoolkit.client import send_command
from diagtoolkit.config import RunnerConfig, ToolkitConfig
from diagtoolkit.log_analysis import analyze_log, choose_next_action


class SessionState:
    BOOTING = "booting"
    XXDP_READY = "xxdp-ready"
    REQUESTING_INPUT = "requesting-input"
    RUNNING_WITH_OUTPUT = "running-with-output"
    RUNNING_SILENT = "running-silent"
    HALTED = "halted"
    CRASHED = "crashed"
    COMPLETE_OR_WAITING = "complete-or-waiting"
    TIMEOUT = "timeout"


@dataclass
class SessionEvent:
    seconds: float
    state: str
    detail: str


@dataclass
class DiagnosticSessionReport:
    diagnostic_command: str
    state: str
    boot_verified: bool
    elapsed_seconds: float
    serial_output: str
    analysis_next_action: str
    events: list[SessionEvent] = field(default_factory=list)

    def as_dict(self) -> dict[str, Any]:
        return {
            "diagnostic_command": self.diagnostic_command,
            "state": self.state,
            "boot_verified": self.boot_verified,
            "elapsed_seconds": round(self.elapsed_seconds, 3),
            "analysis_next_action": self.analysis_next_action,
            "events": [event.__dict__ for event in self.events],
            "serial_output": self.serial_output,
        }


class ManagedServices:
    def __init__(self, toolkit_dir: Path, config_path: Path | None, config: ToolkitConfig):
        self.toolkit_dir = toolkit_dir
        self.config_path = config_path
        self.config = config
        self.processes: list[subprocess.Popen[str]] = []

    def __enter__(self) -> "ManagedServices":
        runner = self.config.runner
        self._maybe_start_service(runner.serial_service)
        self._maybe_start_service(runner.telnet_service)
        self._wait_for_service("serial", runner.startup_timeout_seconds)
        if runner.telnet_service.enabled:
            self._wait_for_service("telnet", runner.startup_timeout_seconds)
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        for service in ("telnet", "serial"):
            try:
                self.command(service, {"command": "exit"}, timeout=2.0)
            except Exception:
                pass
        for process in self.processes:
            try:
                process.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                process.terminate()

    def _maybe_start_service(self, service: Any) -> None:
        if not service.enabled:
            return
        script = self.toolkit_dir / service.script
        command = [service.python, str(script)]
        if self.config_path:
            command.extend(["--config", str(self.config_path)])
        else:
            command.extend(["--config", str(self.toolkit_dir / "config.example.json")])
        process = subprocess.Popen(
            command,
            cwd=str(self.toolkit_dir),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self.processes.append(process)

    def _wait_for_service(self, service: str, timeout_seconds: float) -> None:
        deadline = time.monotonic() + timeout_seconds
        last_error = ""
        while time.monotonic() < deadline:
            try:
                response = self.command(service, {"command": "status"}, timeout=1.0)
                if response.get("ok"):
                    return
                last_error = str(response)
            except Exception as exc:
                last_error = str(exc)
            time.sleep(0.25)
        raise TimeoutError(f"{service} service did not become ready: {last_error}")

    def command(self, service: str, command: dict[str, Any], timeout: float = 10.0) -> dict[str, Any]:
        cfg = self.config.serial if service == "serial" else self.config.telnet
        return send_command(cfg.control_host, cfg.control_port, command, timeout=timeout)


def _looks_like_xxdp_ready(text: str, runner: RunnerConfig) -> bool:
    if runner.xxdp_banner not in text:
        return False
    stripped = text.rstrip()
    return stripped.endswith(runner.prompt) or f'\n{runner.prompt}' in text[-20:]


def _classify(text: str, previous_len: int, runner: RunnerConfig, silent_for: float) -> str:
    if "HALT instruction" in text or "HALT regs" in text:
        return SessionState.HALTED
    if "[vpdp1170 ERR]" in text and "HALT" not in text:
        return SessionState.CRASHED
    if "HIT STOP ON DRIVE" in text or "PROGRAM WILL HANG" in text:
        return SessionState.REQUESTING_INPUT
    if "?" in text[-80:] and text.rstrip().endswith("?"):
        return SessionState.REQUESTING_INPUT
    if len(text) > previous_len:
        return SessionState.RUNNING_WITH_OUTPUT
    if silent_for >= runner.silent_timeout_seconds:
        return SessionState.RUNNING_SILENT
    if text.rstrip().endswith(runner.prompt):
        return SessionState.COMPLETE_OR_WAITING
    return SessionState.RUNNING_SILENT


def run_diagnostic_session(
    config: ToolkitConfig,
    toolkit_dir: Path,
    config_path: Path | None,
    diagnostic_command: str,
) -> DiagnosticSessionReport:
    runner = config.runner
    start = time.monotonic()
    events: list[SessionEvent] = []

    def event(state: str, detail: str) -> None:
        events.append(SessionEvent(round(time.monotonic() - start, 3), state, detail))

    with ManagedServices(toolkit_dir, config_path, config) as services:
        event(SessionState.BOOTING, "services started")
        services.command("serial", {"command": "flush"})
        services.command("telnet", {"command": "flush"})
        services.command("telnet", {"command": "disconnect"})
        services.command("serial", {"command": "reboot-board"})
        event(SessionState.BOOTING, "reboot-board sent")

        boot_deadline = time.monotonic() + runner.startup_timeout_seconds
        boot_text = ""
        while time.monotonic() < boot_deadline:
            time.sleep(runner.boot_poll_seconds)
            response = services.command("serial", {"command": "read", "clear": False})
            boot_text = str(response.get("text", ""))
            if _looks_like_xxdp_ready(boot_text, runner):
                event(SessionState.XXDP_READY, "XXDP banner and prompt detected")
                break
        else:
            analysis = analyze_log(boot_text)
            return DiagnosticSessionReport(
                diagnostic_command=diagnostic_command,
                state=SessionState.TIMEOUT,
                boot_verified=False,
                elapsed_seconds=time.monotonic() - start,
                serial_output=boot_text,
                analysis_next_action=choose_next_action(analysis),
                events=events,
            )

        telnet_deadline = time.monotonic() + runner.startup_timeout_seconds
        while time.monotonic() < telnet_deadline:
            response = services.command("telnet", {"command": "connect", "required": False})
            if response.get("connected"):
                event(SessionState.XXDP_READY, "telnet reconnected after board reboot")
                break
            time.sleep(1.0)

        command_text = diagnostic_command
        if not command_text.endswith("\r"):
            command_text += "\r"
        services.command("serial", {"command": "send", "text": command_text})
        event(SessionState.RUNNING_WITH_OUTPUT, f"sent diagnostic command: {diagnostic_command}")

        last_len = len(boot_text)
        last_output_time = time.monotonic()
        final_state = SessionState.RUNNING_SILENT
        serial_text = boot_text
        run_deadline = time.monotonic() + runner.max_run_seconds

        while time.monotonic() < run_deadline:
            time.sleep(runner.diagnostic_poll_seconds)
            response = services.command("serial", {"command": "read", "clear": False})
            serial_text = str(response.get("text", ""))
            if len(serial_text) > last_len:
                last_output_time = time.monotonic()
            silent_for = time.monotonic() - last_output_time
            state = _classify(serial_text, last_len, runner, silent_for)
            if not events or events[-1].state != state:
                event(state, f"serial chars={len(serial_text)}, silent_for={silent_for:.1f}s")
            final_state = state
            last_len = len(serial_text)
            if state in (
                SessionState.REQUESTING_INPUT,
                SessionState.HALTED,
                SessionState.CRASHED,
                SessionState.COMPLETE_OR_WAITING,
            ):
                break

        if time.monotonic() >= run_deadline:
            final_state = SessionState.TIMEOUT
            event(final_state, "diagnostic run timeout reached")

        analysis = analyze_log(serial_text)
        return DiagnosticSessionReport(
            diagnostic_command=diagnostic_command,
            state=final_state,
            boot_verified=True,
            elapsed_seconds=time.monotonic() - start,
            serial_output=serial_text,
            analysis_next_action=choose_next_action(analysis),
            events=events,
        )


def default_toolkit_dir() -> Path:
    return Path(__file__).resolve().parents[1]
