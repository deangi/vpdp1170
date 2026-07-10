#!/usr/bin/env python3
"""Capture KL11 console_trace boot logs over FTP + serial for Phase 2 analysis.

Usage:
  python tools/capture_kl11_boot.py --all
  python tools/capture_kl11_boot.py --os rstsv4b
"""

from __future__ import annotations

import argparse
import io
import re
import sys
import time
from collections import Counter
from ftplib import FTP, error_perm
from pathlib import Path

try:
    import serial
except ImportError:
    print("pyserial required: pip install pyserial", file=sys.stderr)
    sys.exit(1)

ROOT = Path(__file__).resolve().parents[1]
TRACES = ROOT / "documentation" / "os-console" / "traces"

FTP_HOST = "192.168.7.144"
FTP_PORT = 21
FTP_CREDS = [
    ("anonymous", ""),
    ("anonymous", "anonymous"),
    ("esp32", "esp32"),
    ("esp32", ""),
    ("", ""),
]

SERIAL_PORT = "COM18"
SERIAL_BAUD = 115200
CAPTURE_SECS = 30.0

OS_ORDER = [
    ("rstsv4b", "pdpconfig-rstsv4.ini"),
    ("rt11v5", "pdpconfig-rt11v5.ini"),
    ("unixv6", "pdpconfig-unixv6.ini"),
    ("rsx11m", "pdpconfig-rsx11m.ini"),
    ("11mark", "pdpconfig-11mark.ini"),
]

KEK_TTY_RE = re.compile(
    r"kek TTY\s+(\S+)\s+(\S+)\s+@\s+(\d+)\s+val=(\d+)(?:\s+(.*?))?\s+remaining=(\d+)"
)
CONSOLE_RE = re.compile(r"CONSOLE\s+(\S+)\s+char=(\d+)")
TRACE_ENDED_RE = re.compile(r"kek TTY trace ended:")

def ftp_connect() -> FTP:
    last_err: Exception | None = None
    for user, passwd in FTP_CREDS:
        ftp = FTP()
        try:
            ftp.connect(FTP_HOST, FTP_PORT, timeout=15)
            ftp.login(user or "anonymous", passwd)
            ftp.set_pasv(True)
            print(f"FTP login ok as {user!r}")
            return ftp
        except Exception as e:
            last_err = e
            try:
                ftp.close()
            except Exception:
                pass
    raise RuntimeError(f"FTP login failed for {FTP_HOST}: {last_err}")

def ftp_list(ftp: FTP) -> list[str]:
    try:
        return ftp.nlst()
    except error_perm:
        names: list[str] = []
        ftp.retrlines("LIST", names.append)
        return names


def ftp_find_path(ftp: FTP, filename: str) -> str:
    names = ftp_list(ftp)
    bare = [n.split("/")[-1] for n in names]
    if filename in bare or filename in names:
        return filename
    for candidate in (f"/{filename}", f"./{filename}"):
        try:
            ftp.size(candidate)
            return candidate
        except Exception:
            continue
    return filename


def ftp_retr_text(ftp: FTP, path: str) -> str:
    buf = io.BytesIO()
    ftp.retrbinary(f"RETR {path}", buf.write)
    return buf.getvalue().decode("utf-8", errors="replace")


def ftp_stor_text(ftp: FTP, path: str, text: str) -> None:
    data = text.encode("utf-8")
    ftp.storbinary(f"STOR {path}", io.BytesIO(data))

def ensure_console_trace(ini_text: str, count: int = 1000) -> str:
    lines = ini_text.replace("\r\n", "\n").replace("\r", "\n").split("\n")
    out: list[str] = []
    in_diag = False
    saw_console = False
    i = 0
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            if in_diag and not saw_console:
                out.append(f"console_trace = {count}")
                saw_console = True
            section = stripped[1:-1].strip().lower()
            in_diag = section == "diag"
            out.append(line)
            i += 1
            continue
        if in_diag and re.match(r"(?i)^\s*console_trace\s*=", line):
            out.append(f"console_trace = {count}")
            saw_console = True
            i += 1
            continue
        out.append(line)
        i += 1
    if in_diag and not saw_console:
        insert_at = len(out)
        while insert_at > 0 and out[insert_at - 1].strip() == "":
            insert_at -= 1
        out.insert(insert_at, f"console_trace = {count}")
        saw_console = True
    if not saw_console:
        if out and out[-1].strip() != "":
            out.append("")
        out.append("[diag]")
        out.append(f"console_trace = {count}")
    text = "\n".join(out)
    if not text.endswith("\n"):
        text += "\n"
    return text.replace("\n", "\r\n")

def reboot_and_capture(out_path: Path, seconds: float = CAPTURE_SECS) -> str:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    print(f"  Serial: open {SERIAL_PORT} (reboot trigger)...")
    ser = serial.Serial(SERIAL_PORT, SERIAL_BAUD, timeout=0.2)
    time.sleep(1.0)
    ser.close()
    print("  Serial: closed; waiting 4s for boot...")
    time.sleep(4.0)
    print(f"  Serial: reopen; capturing {seconds:.0f}s -> {out_path.name}")
    chunks: list[bytes] = []
    ser = serial.Serial(SERIAL_PORT, SERIAL_BAUD, timeout=0.25)
    try:
        # Do not flush input — keep bytes already queued after reopen.
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            n = ser.in_waiting
            if n:
                chunks.append(ser.read(n))
            else:
                data = ser.read(256)
                if data:
                    chunks.append(data)
                else:
                    time.sleep(0.05)
    finally:
        ser.close()
    raw = b"".join(chunks)
    text = raw.decode("utf-8", errors="replace")
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    out_path.write_text(text, encoding="utf-8", errors="replace")
    print(f"  Captured {len(raw)} bytes, {text.count(chr(10)) + 1} lines")
    return text

def parse_registers(log_text: str) -> dict:
    events = []
    by_action: Counter[str] = Counter()
    by_reg: Counter[str] = Counter()
    by_pair: Counter[str] = Counter()
    writes: list[dict] = []
    irqs: list[dict] = []
    reads: list[dict] = []
    console_chars = 0
    trace_ended = False
    for line in log_text.splitlines():
        if TRACE_ENDED_RE.search(line):
            trace_ended = True
        if CONSOLE_RE.search(line):
            console_chars += 1
        m = KEK_TTY_RE.search(line)
        if not m:
            continue
        action, reg, addr, val, detail, rem = m.groups()
        detail = (detail or "").strip()
        ev = {
            "action": action, "reg": reg, "addr": addr, "val": val,
            "detail": detail, "remaining": int(rem), "line": line.strip(),
        }
        events.append(ev)
        by_action[action] += 1
        by_reg[reg] += 1
        by_pair[f"{action} {reg}"] += 1
        if action == "WRITE":
            writes.append(ev)
        elif action == "READ":
            reads.append(ev)
        elif action == "IRQ":
            irqs.append(ev)
    tks_writes = [e for e in writes if e["reg"] == "TKS"]
    tps_writes = [e for e in writes if e["reg"] == "TPS"]
    tps_reads = [e for e in reads if e["reg"] == "TPS"]
    tkb_reads = [e for e in reads if e["reg"] == "TKB"]
    tks_reads = [e for e in reads if e["reg"] == "TKS"]
    return {
        "events": events, "by_action": by_action, "by_reg": by_reg, "by_pair": by_pair,
        "writes": writes, "reads": reads, "irqs": irqs,
        "console_chars": console_chars, "trace_ended": trace_ended,
        "tks_write_vals": [e["val"] for e in tks_writes[:20]],
        "tps_write_vals": [e["val"] for e in tps_writes[:20]],
        "tps_read_count": len(tps_reads), "tks_read_count": len(tks_reads),
        "tkb_read_count": len(tkb_reads),
        "irq_details": Counter(e["detail"] for e in irqs),
    }


def write_registers_md(os_key: str, parsed: dict, out_path: Path) -> None:
    lines = [
        f"# {os_key} KL11 register activity",
        "",
        f"Parsed from `{os_key}-console.log`.",
        "",
        "## Summary",
        "",
        f"- Total kek TTY events: **{len(parsed['events'])}**",
        f"- CONSOLE char lines: **{parsed['console_chars']}**",
        f"- Trace ended marker: **{parsed['trace_ended']}**",
        "",
        "### By action",
        "",
    ]
    if parsed["by_action"]:
        lines += ["| Action | Count |", "|--------|------:|"]
        for k, v in parsed["by_action"].most_common():
            lines.append(f"| {k} | {v} |")
    else:
        lines.append("_No kek TTY lines found._")
    lines += ["", "### By register", ""]
    if parsed["by_reg"]:
        lines += ["| Reg | Count |", "|-----|------:|"]
        for k, v in parsed["by_reg"].most_common():
            lines.append(f"| {k} | {v} |")
    lines += ["", "### Action x register", ""]
    if parsed["by_pair"]:
        lines += ["| Pair | Count |", "|------|------:|"]
        for k, v in parsed["by_pair"].most_common():
            lines.append(f"| {k} | {v} |")
    lines += ["", "### IRQ details", ""]
    if parsed["irq_details"]:
        lines += ["| Detail | Count |", "|--------|------:|"]
        for k, v in parsed["irq_details"].most_common():
            lines.append(f"| `{k}` | {v} |")
    else:
        lines.append("_None._")
    lines += [
        "",
        "### Notable CSR traffic",
        "",
        f"- TKS WRITE values (first): `{', '.join(parsed['tks_write_vals']) or 'none'}`",
        f"- TPS WRITE values (first): `{', '.join(parsed['tps_write_vals']) or 'none'}`",
        f"- TKS READ count: {parsed['tks_read_count']}",
        f"- TPS READ count: {parsed['tps_read_count']}",
        f"- TKB READ count: {parsed['tkb_read_count']}",
        "",
        "## Sample events (first 40)",
        "",
    ]
    for ev in parsed["events"][:40]:
        lines.append(f"- `{ev['line']}`")
    if len(parsed["events"]) > 40:
        lines.append(f"- ... ({len(parsed['events']) - 40} more)")
    lines.append("")
    out_path.write_text("\n".join(lines), encoding="utf-8")

def capture_one(os_key: str, ini_name: str) -> dict:
    result = {
        "os": os_key, "ini": ini_name, "ok": False, "error": None,
        "log": TRACES / f"{os_key}-console.log",
        "regs": TRACES / f"{os_key}-registers.md",
        "parsed": None, "sample": [],
    }
    print(f"\n=== {os_key} ({ini_name}) ===")
    ftp = None
    try:
        ftp = ftp_connect()
        src = ftp_find_path(ftp, ini_name)
        print(f"  FTP RETR {src}")
        ini = ftp_retr_text(ftp, src)
        patched = ensure_console_trace(ini, 1000)
        print("  FTP STOR pdpconfig.ini (console_trace=1000)")
        ftp_stor_text(ftp, "pdpconfig.ini", patched)
        try:
            ftp.quit()
        except Exception:
            ftp.close()
        ftp = None
        log_text = reboot_and_capture(result["log"], CAPTURE_SECS)
        parsed = parse_registers(log_text)
        result["parsed"] = parsed
        result["sample"] = [e["line"] for e in parsed["events"][:8]]
        write_registers_md(os_key, parsed, result["regs"])
        print(
            f"  Events: {len(parsed['events'])} "
            f"(READ={parsed['by_action'].get('READ', 0)} "
            f"WRITE={parsed['by_action'].get('WRITE', 0)} "
            f"IRQ={parsed['by_action'].get('IRQ', 0)})"
        )
        result["ok"] = True
    except Exception as e:
        result["error"] = str(e)
        print(f"  FAIL: {e}")
    finally:
        if ftp is not None:
            try:
                ftp.close()
            except Exception:
                pass
    return result


def write_kl11_by_os(results: list[dict]) -> Path:
    out = ROOT / "documentation" / "os-console" / "KL11_BY_OS.md"
    lines = [
        "# KL11 console_trace by OS (Phase 2)",
        "",
        "Live captures via `tools/capture_kl11_boot.py --all` "
        f"(COM18 @ 115200, FTP `{FTP_HOST}`, `console_trace = 1000`).",
        "",
        "Raw logs and per-OS register digests live under "
        "`documentation/os-console/traces/`.",
        "",
        "## Capture status",
        "",
        "| OS | Status | kek TTY events | READ | WRITE | IRQ | Notes |",
        "|----|--------|---------------:|-----:|------:|----:|-------|",
    ]
    for r in results:
        p = r.get("parsed") or {}
        ba = p.get("by_action") or Counter()
        status = "OK" if r["ok"] else "FAIL"
        notes = r.get("error") or (
            f"TKS wr={','.join((p.get('tks_write_vals') or [])[:4]) or '-'} "
            f"TPS rd={p.get('tps_read_count', 0)}"
        )
        lines.append(
            f"| {r['os']} | {status} | {len(p.get('events') or [])} | "
            f"{ba.get('READ', 0)} | {ba.get('WRITE', 0)} | {ba.get('IRQ', 0)} | "
            f"{notes} |"
        )
    lines += ["", "## Register patterns per OS", ""]
    for r in results:
        lines.append(f"### {r['os']}")
        lines.append("")
        if not r["ok"]:
            lines.append(f"_Capture failed: {r.get('error')}_")
            lines.append("")
            continue
        p = r["parsed"]
        lines.append(
            f"- See [`traces/{r['os']}-registers.md`]"
            f"(traces/{r['os']}-registers.md) / "
            f"[`traces/{r['os']}-console.log`](traces/{r['os']}-console.log)."
        )
        lines.append(
            "- Action mix: "
            + ", ".join(f"{k}={v}" for k, v in p["by_action"].most_common())
        )
        lines.append(
            f"- TKS WRITE sample: `{', '.join(p['tks_write_vals'][:8]) or 'none'}`"
        )
        lines.append(
            f"- TPS WRITE sample: `{', '.join(p['tps_write_vals'][:8]) or 'none'}`"
        )
        lines.append(
            f"- Polling: TKS READ={p['tks_read_count']}, "
            f"TPS READ={p['tps_read_count']}, TKB READ={p['tkb_read_count']}"
        )
        if p["irq_details"]:
            irq_s = ", ".join(f"{k}x{v}" for k, v in p["irq_details"].most_common(6))
            lines.append(f"- IRQ: {irq_s}")
        if r["os"] == "unixv6":
            has_rdr = any(v[-1:] in "13" for v in p["tks_write_vals"])
            lines.append(
                f"- Unix RDR ENB after RBUF: "
                f"{'likely present' if has_rdr or p['tkb_read_count'] else 'not obvious in window'} "
                f"(TKB reads={p['tkb_read_count']})."
            )
        if r["os"] == "rstsv4b":
            lines.append(
                "- RSTS KB01: watch for CSR probes / IE-only writes that must "
                "preserve DONE (see TKS WRITE values above)."
            )
        if r["os"] in ("rsx11m", "11mark"):
            lines.append(
                "- RSX/11mark: pattern from this capture only — compare IE "
                "enable vs polled TPS/TKS density."
            )
        lines.append("")
        if r["sample"]:
            lines.append("Sample events:")
            lines.append("")
            for s in r["sample"][:5]:
                lines.append(f"  - `{s}`")
            lines.append("")
    lines += [
        "## Spec gap vs proposed `kek_src_tty` rules",
        "",
        "Evidence is from these Phase 2 traces (and ANALYSIS.md expectations).",
        "",
        "| Rule | Intent | Trace evidence / gap |",
        "|------|--------|----------------------|",
        "| IE-only CSR writes preserve DONE | Writing IE (0100) must not clear bit7 DONE | Inspect TKS/TPS WRITE vals while DONE should stay set; RSTS KB01 especially |",
        "| Bit 7 only | Software tests DONE via `0200` / TSTB, not bit15 | TPS/TKS READ vals should show `02xx` when ready |",
        "| Cheap TPS read | Polled putchar / RT-11 loops hammer TPS READ | High TPS READ counts (RT-11, Unix printf) expected |",
        "| IE while DONE must IRQ | Enabling IE with DONE already 1 queues vec 060/064 | Look for `IRQ` queue vec=064 soon after TPS WRITE IE |",
        "| No TX requeue storm | Must not endlessly `requeue vec=064` | `requeue` detail counts should stay low vs queue/unqueue |",
        "| RDR ENB after RBUF (Unix) | After TKB READ, TKS WRITE with bit0 | unixv6: TKB READ then TKS WRITE `...1` / `0103` |",
        "| RSTS KB01 CSR preserve | Probe must not wipe live console CSR bits | rstsv4b: IE-ish writes + continued console traffic |",
        "| RSX / 11mark from traces | Fill from live RSX patterns | rsx11m + 11mark sections above |",
        "",
        "### Observed snapshot (this run)",
        "",
    ]
    for r in results:
        if not r["ok"]:
            continue
        p = r["parsed"]
        requeue = sum(v for k, v in p["irq_details"].items() if "requeue" in k)
        queue = sum(
            v for k, v in p["irq_details"].items()
            if "queue" in k and "requeue" not in k and "unqueue" not in k
        )
        lines.append(
            f"- **{r['os']}**: TPS READ={p['tps_read_count']}, "
            f"TKS WRITE n={len([e for e in p['writes'] if e['reg']=='TKS'])}, "
            f"IRQ queue~{queue} requeue~{requeue}, "
            f"TKB READ={p['tkb_read_count']}"
        )
    lines.append("")
    out.write_text("\n".join(lines), encoding="utf-8")
    return out


def main(argv: list[str] | None = None) -> int:
    global CAPTURE_SECS
    ap = argparse.ArgumentParser(description="KL11 console_trace boot capture")
    ap.add_argument("--os", choices=[k for k, _ in OS_ORDER], help="Capture one OS")
    ap.add_argument("--all", action="store_true", help="Capture all OSes in order")
    ap.add_argument("--list-ftp", action="store_true", help="List FTP root and exit")
    ap.add_argument("--seconds", type=float, default=CAPTURE_SECS, help="Capture seconds")
    args = ap.parse_args(argv)
    CAPTURE_SECS = args.seconds
    TRACES.mkdir(parents=True, exist_ok=True)
    if args.list_ftp:
        ftp = ftp_connect()
        print("FTP listing:")
        for n in ftp_list(ftp):
            print(" ", n)
        ftp.quit()
        return 0
    if not args.all and not args.os:
        ap.error("specify --all or --os NAME")
    targets = OS_ORDER if args.all else [(k, v) for k, v in OS_ORDER if k == args.os]
    results = []
    for os_key, ini_name in targets:
        results.append(capture_one(os_key, ini_name))
    summary_path = write_kl11_by_os(results)
    print("\n======== SUMMARY ========")
    for r in results:
        flag = "OK" if r["ok"] else "FAIL"
        n = len((r.get("parsed") or {}).get("events") or [])
        print(f"  {r['os']}: {flag}  events={n}  err={r.get('error')}")
        for s in (r.get("sample") or [])[:3]:
            print(f"    {s}")
    print(f"KL11_BY_OS.md -> {summary_path}")
    return 0 if all(r["ok"] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
