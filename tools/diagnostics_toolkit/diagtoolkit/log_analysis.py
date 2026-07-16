"""Lightweight analysis for vpdp1170 diagnostic logs."""

from __future__ import annotations

from dataclasses import dataclass, field
import re


DIAGNOSTIC_RE = re.compile(r"\b(C?Z[A-Z0-9]{4,6})\s*-\s*(.+)")
RUN_RE = re.compile(r"^\s*\.R\s+(\S+)", re.MULTILINE)
FAILURE_RE = re.compile(
    r"WRONG DATA IN READING OR WRITING HARDWARE REGISTER.*?"
    r"(?P<pc>[0-7]{6})\s+(?P<test>[0-7]{6})\s+"
    r"(?P<reg>[0-7]{6})\s+(?P<good>[0-7]{6})\s+(?P<received>[0-7]{6})",
    re.DOTALL,
)


@dataclass(frozen=True)
class RegisterFailure:
    pc: str
    test: str
    register: str
    expected: str
    received: str


@dataclass
class LogAnalysis:
    run_commands: list[str] = field(default_factory=list)
    diagnostics: list[str] = field(default_factory=list)
    manual_prompts: list[str] = field(default_factory=list)
    register_failures: list[RegisterFailure] = field(default_factory=list)
    emulator_errors: list[str] = field(default_factory=list)
    halts: list[str] = field(default_factory=list)
    interrupts: list[str] = field(default_factory=list)
    raw_line_count: int = 0


def _clean_line(line: str) -> str:
    return line.replace("\u25a1", "").strip()


def analyze_log(text: str) -> LogAnalysis:
    lines = [_clean_line(line) for line in text.splitlines()]
    analysis = LogAnalysis(raw_line_count=len(lines))

    analysis.run_commands.extend(RUN_RE.findall(text))

    for line in lines:
        if not line:
            continue
        diag = DIAGNOSTIC_RE.search(line)
        if diag:
            analysis.diagnostics.append(f"{diag.group(1)} - {diag.group(2).strip()}")
        if "HIT STOP ON DRIVE" in line or "HIT START" in line:
            analysis.manual_prompts.append(line)
        if "PROGRAM WILL HANG" in line:
            analysis.manual_prompts.append(line)
        if "[vpdp1170 ERR]" in line:
            analysis.emulator_errors.append(line)
        if "HALT" in line:
            analysis.halts.append(line)
        if "UNEXPECTED RP INT" in line or "INTERRUPT" in line:
            analysis.interrupts.append(line)

    for match in FAILURE_RE.finditer(text):
        analysis.register_failures.append(
            RegisterFailure(
                pc=match.group("pc"),
                test=match.group("test"),
                register=match.group("reg"),
                expected=match.group("good"),
                received=match.group("received"),
            )
        )

    return analysis


def choose_next_action(analysis: LogAnalysis) -> str:
    prompt_text = "\n".join(analysis.manual_prompts)
    if "HIT STOP ON DRIVE" in prompt_text or "MOL TILL MOL IS LOW" in prompt_text:
        return "Next action: enter the telnet management shell and run `rp stop`."

    if analysis.register_failures:
        first = analysis.register_failures[0]
        return (
            "Next action: inspect RP/RH register emulation. "
            f"First mismatch is register {first.register}: "
            f"expected {first.expected}, received {first.received} "
            f"at PC {first.pc}, test {first.test}."
        )

    if analysis.interrupts:
        return "Next action: inspect RP/RH interrupt behavior and pending IRQ clearing."

    if analysis.halts:
        return "Next action: inspect the HALT context and the preceding diagnostic error."

    if analysis.emulator_errors:
        return "Next action: resolve the emulator-side error before trusting diagnostic output."

    return "Next action: continue the diagnostic run or capture more log output."


def format_summary(analysis: LogAnalysis) -> str:
    out: list[str] = []
    out.append("Diagnostics Toolkit Summary")
    out.append("===========================")
    out.append(f"Lines: {analysis.raw_line_count}")

    if analysis.run_commands:
        out.append("")
        out.append("Run commands:")
        out.extend(f"- {cmd}" for cmd in analysis.run_commands)

    if analysis.diagnostics:
        out.append("")
        out.append("Diagnostics:")
        out.extend(f"- {diag}" for diag in analysis.diagnostics)

    if analysis.manual_prompts:
        out.append("")
        out.append("Manual prompts:")
        out.extend(f"- {prompt}" for prompt in analysis.manual_prompts)

    if analysis.register_failures:
        out.append("")
        out.append(f"Register failures: {len(analysis.register_failures)}")
        for failure in analysis.register_failures[:10]:
            out.append(
                "- "
                f"PC={failure.pc} TEST={failure.test} REG={failure.register} "
                f"GOOD={failure.expected} RECEIVED={failure.received}"
            )
        if len(analysis.register_failures) > 10:
            out.append(f"- ... {len(analysis.register_failures) - 10} more")

    if analysis.interrupts:
        out.append("")
        out.append("Interrupt notes:")
        out.extend(f"- {line}" for line in analysis.interrupts[:5])

    if analysis.halts:
        out.append("")
        out.append("HALTs:")
        out.extend(f"- {line}" for line in analysis.halts[:5])

    if analysis.emulator_errors:
        out.append("")
        out.append("Emulator errors:")
        out.extend(f"- {line}" for line in analysis.emulator_errors[:5])

    out.append("")
    out.append(choose_next_action(analysis))
    return "\n".join(out)
