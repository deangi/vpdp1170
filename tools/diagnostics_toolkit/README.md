# Diagnostics Toolkit

Tools for running and interpreting PDP-11 diagnostics on `vpdp1170`.

The first target is XXDP diagnostic work against emulated devices such as
RL02, RH70/RP04/RP05/RP06, RK05, CPU, and MMU. The toolkit is deliberately
host-side: it parses logs, suggests operator actions, and records runbooks
without requiring an emulator rebuild.

## Goals

- Turn pasted serial/telnet logs into concise diagnostic summaries.
- Detect manual-action waits such as RP drive STOP/START prompts.
- Classify failures by probable area: diagnostic setup, expected operator
  action, controller register behavior, DMA/data path, interrupt behavior, or
  guest HALT.
- Keep device-specific runbooks close to the repo so repeated diagnostic runs
  become reproducible.
- Produce small, shareable summaries that can be pasted back into Codex or a
  GitHub issue.

## Layout

```text
tools/diagnostics_toolkit/
  dtk.py                 command-line entry point
  diagtoolkit/           parser and analysis modules
  runbooks/              diagnostic runbooks by subsystem
  samples/               tiny parser fixtures, not full logs
```

## Usage

From the `vpdp1170` repo root:

```powershell
python tools\diagnostics_toolkit\dtk.py analyze-log path\to\capture.txt
python tools\diagnostics_toolkit\dtk.py next-action path\to\capture.txt
python tools\diagnostics_toolkit\dtk.py run-diagnostic "R ZRJAD0" --config tools\diagnostics_toolkit\config.example.json
```

## Initial Commands

`analyze-log`

Summarizes the diagnostic name, manual prompts, failure tables, emulator
errors, and HALT records.

`next-action`

Prints the highest-priority recommendation inferred from the log. For example,
an RP diagnostic waiting for `MOL` low should recommend `rp stop` in the
telnet management shell.

## Near-Term Roadmap

1. Harden serial reset timing against the actual ESP32-S3 board behavior.
2. Add runbook metadata for common diagnostics (`ZRJGE0`, `ZRJHE0`, `ZRJAD0`,
   `ZRLHB1`).
3. Add optional telnet automation for operator actions, gated behind explicit
   command-line flags.
4. Add a diagnostic session report format with exact command history and
   firmware commit IDs.

## Service Architecture

The toolkit uses two long-running services so only one process owns each live
connection.

`SerialComs`

- Owns the serial port (`COM18`, `115200` baud by default).
- Accepts JSON-lines commands on localhost port `11701`.
- Commands: `status`, `reboot-board`, `send`, `read`, `flush`, `clear`, `exit`.
- `reboot-board` leaves DTR high, pulses RTS low for 0.5 seconds, then leaves
  RTS high.

`TelnetComs`

- Owns a telnet connection to the board (`192.168.7.144:23` by default).
- Accepts JSON-lines commands on localhost port `11702`.
- Commands: `status`, `connect`, `disconnect`, `send`, `read`, `flush`,
  `clear`, `enter-shell`, `enter-monitor`, `exit-monitor`, `exit-shell`,
  `exit`.
- Board reset disconnects telnet. The service stays alive while disconnected,
  and the runner reconnects after the board has rebooted.
- Telnet modes and transitions:
  - PDP to telnet shell: `enter-shell` sends `ESC>>`.
  - Telnet shell to monitor: `enter-monitor` sends `monitor<CR>`.
  - Monitor to telnet shell: `exit-monitor` sends `><CR>`.
  - Telnet shell to PDP: `exit-shell` sends `exit<CR>`.

`run-diagnostic`

- Starts both services.
- Sends `reboot-board` to `SerialComs`.
- Waits for the XXDP V2.5 banner and `.` prompt.
- Sends the requested diagnostic command, appending carriage return if needed.
- Polls serial output and classifies the run as requesting input, running with
  output, running silently, HALTed, crashed, complete/waiting, or timed out.

## Firmware Rebuild/Upload

The first version of the runner assumes the board is already flashed. The next
layer should add explicit build/upload commands using Arduino CLI before the
session starts, for example:

```powershell
arduino-cli compile ...
arduino-cli upload ...
```

Keep build/upload as an explicit runner option so log-only diagnostic sessions
do not accidentally rewrite the board.
