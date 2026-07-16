"""Configuration loading for Diagnostics Toolkit services."""

from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path


@dataclass(frozen=True)
class ResetConfig:
    method: str = "dtr_rts"
    pulse_seconds: float = 0.50
    settle_seconds: float = 0.50


@dataclass(frozen=True)
class SerialConfig:
    port: str = "COM18"
    baud: int = 115200
    control_host: str = "127.0.0.1"
    control_port: int = 11701
    read_buffer_limit: int = 1024 * 1024
    reset: ResetConfig = ResetConfig()


@dataclass(frozen=True)
class TelnetConfig:
    target_host: str = "192.168.7.144"
    target_port: int = 23
    control_host: str = "127.0.0.1"
    control_port: int = 11702
    read_buffer_limit: int = 1024 * 1024


@dataclass(frozen=True)
class ServiceProcessConfig:
    enabled: bool = True
    python: str = "python"
    script: str = ""


@dataclass(frozen=True)
class RunnerConfig:
    startup_timeout_seconds: float = 20.0
    boot_poll_seconds: float = 1.0
    diagnostic_poll_seconds: float = 5.0
    silent_timeout_seconds: float = 60.0
    max_run_seconds: float = 900.0
    xxdp_banner: str = "XXDP-XM EXTENDED MONITOR - XXDP V2.5"
    prompt: str = "."
    serial_service: ServiceProcessConfig = ServiceProcessConfig(script="services/serialcoms_service.py")
    telnet_service: ServiceProcessConfig = ServiceProcessConfig(script="services/telnetcoms_service.py")


@dataclass(frozen=True)
class ToolkitConfig:
    serial: SerialConfig = SerialConfig()
    telnet: TelnetConfig = TelnetConfig()
    runner: RunnerConfig = RunnerConfig()


def load_config(path: Path | None) -> ToolkitConfig:
    if path is None:
        return ToolkitConfig()

    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise SystemExit(f"error: cannot read config {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise SystemExit(f"error: invalid JSON config {path}: {exc}") from exc

    serial_data = data.get("serial", {})
    reset_data = serial_data.get("reset", {})
    telnet_data = data.get("telnet", {})
    runner_data = data.get("runner", {})
    serial_service_data = runner_data.get("serial_service", {})
    telnet_service_data = runner_data.get("telnet_service", {})

    return ToolkitConfig(
        serial=SerialConfig(
            port=str(serial_data.get("port", "COM18")),
            baud=int(serial_data.get("baud", 115200)),
            control_host=str(serial_data.get("control_host", "127.0.0.1")),
            control_port=int(serial_data.get("control_port", 11701)),
            read_buffer_limit=int(serial_data.get("read_buffer_limit", 1024 * 1024)),
            reset=ResetConfig(
                method=str(reset_data.get("method", "dtr_rts")),
                pulse_seconds=float(reset_data.get("pulse_seconds", 0.50)),
                settle_seconds=float(reset_data.get("settle_seconds", 0.50)),
            ),
        ),
        telnet=TelnetConfig(
            target_host=str(telnet_data.get("target_host", "192.168.7.144")),
            target_port=int(telnet_data.get("target_port", 23)),
            control_host=str(telnet_data.get("control_host", "127.0.0.1")),
            control_port=int(telnet_data.get("control_port", 11702)),
            read_buffer_limit=int(telnet_data.get("read_buffer_limit", 1024 * 1024)),
        ),
        runner=RunnerConfig(
            startup_timeout_seconds=float(runner_data.get("startup_timeout_seconds", 20.0)),
            boot_poll_seconds=float(runner_data.get("boot_poll_seconds", 1.0)),
            diagnostic_poll_seconds=float(runner_data.get("diagnostic_poll_seconds", 5.0)),
            silent_timeout_seconds=float(runner_data.get("silent_timeout_seconds", 60.0)),
            max_run_seconds=float(runner_data.get("max_run_seconds", 900.0)),
            xxdp_banner=str(
                runner_data.get("xxdp_banner", "XXDP-XM EXTENDED MONITOR - XXDP V2.5")
            ),
            prompt=str(runner_data.get("prompt", ".")),
            serial_service=ServiceProcessConfig(
                enabled=bool(serial_service_data.get("enabled", True)),
                python=str(serial_service_data.get("python", "python")),
                script=str(serial_service_data.get("script", "services/serialcoms_service.py")),
            ),
            telnet_service=ServiceProcessConfig(
                enabled=bool(telnet_service_data.get("enabled", True)),
                python=str(telnet_service_data.get("python", "python")),
                script=str(telnet_service_data.get("script", "services/telnetcoms_service.py")),
            ),
        ),
    )
