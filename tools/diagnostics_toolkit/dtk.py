#!/usr/bin/env python3
"""Diagnostics Toolkit command-line entry point."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

from diagtoolkit.log_analysis import analyze_log, choose_next_action, format_summary
from diagtoolkit.config import load_config
from diagtoolkit.session_runner import default_toolkit_dir, run_diagnostic_session


def _read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        raise SystemExit(f"error: cannot read {path}: {exc}") from exc


def cmd_analyze_log(args: argparse.Namespace) -> int:
    analysis = analyze_log(_read_text(args.logfile))
    print(format_summary(analysis))
    return 0


def cmd_next_action(args: argparse.Namespace) -> int:
    analysis = analyze_log(_read_text(args.logfile))
    print(choose_next_action(analysis))
    return 0


def cmd_run_diagnostic(args: argparse.Namespace) -> int:
    config = load_config(args.config)
    report = run_diagnostic_session(
        config=config,
        toolkit_dir=default_toolkit_dir(),
        config_path=args.config,
        diagnostic_command=args.command,
    )
    if args.json:
        print(json.dumps(report.as_dict(), indent=2))
    else:
        print(f"State: {report.state}")
        print(f"Boot verified: {report.boot_verified}")
        print(f"Elapsed seconds: {report.elapsed_seconds:.1f}")
        print("")
        print("Events:")
        for event in report.events:
            print(f"- {event.seconds:7.3f}s {event.state}: {event.detail}")
        print("")
        print(report.analysis_next_action)
    return 0 if report.boot_verified and report.state != "timeout" else 1


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="dtk",
        description="Host-side tools for vpdp1170 diagnostic runs.",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    analyze = subparsers.add_parser(
        "analyze-log",
        help="summarize an XXDP/emulator diagnostic log",
    )
    analyze.add_argument("logfile", type=Path)
    analyze.set_defaults(func=cmd_analyze_log)

    next_action = subparsers.add_parser(
        "next-action",
        help="recommend the next operator or emulator action",
    )
    next_action.add_argument("logfile", type=Path)
    next_action.set_defaults(func=cmd_next_action)

    run_diag = subparsers.add_parser(
        "run-diagnostic",
        help="start services, reboot board, boot XXDP, and run a diagnostic",
    )
    run_diag.add_argument("command", help="XXDP command, for example 'R ZRJAD0'")
    run_diag.add_argument("--config", type=Path)
    run_diag.add_argument("--json", action="store_true", help="print structured session report")
    run_diag.set_defaults(func=cmd_run_diagnostic)

    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
