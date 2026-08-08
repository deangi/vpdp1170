#!/usr/bin/env python3
"""Break at 026532; dump softc fields; step past COM to find 002000 reload."""

from __future__ import annotations

import re
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from benchmark_boot_times import (  # noqa: E402
    TelnetConnection,
    BenchmarkError,
    SHELL_PROMPT_RE,
    SHELL_BANNER,
)

HOST = "192.168.7.144"
COM = "COM18"
OUT = ROOT / "boot-benchmark-results-211bsd-postphys"
BP = "026532"
SOFTC = 0o077122
MAX_HITS = 6

BREAK_RE = re.compile(
    r"kek BREAK pc=([0-7]+) PS=([0-7]+) "
    r"R0=([0-7]+) R1=([0-7]+) R2=([0-7]+) R3=([0-7]+) "
    r"R4=([0-7]+) R5=([0-7]+) SP=([0-7]+)"
)
STATE_RE = re.compile(
    r"state: PC=([0-7]+) R0=([0-7]+) R1=([0-7]+) R2=([0-7]+) "
    r"R3=([0-7]+) R4=([0-7]+) R5=([0-7]+) SP=([0-7]+) PS=([0-7]+) "
    r"NEXT=([0-7]+):([0-7]+)\s+(.*)"
)


def drain(conn: TelnetConnection, seconds: float) -> bytes:
    deadline = time.monotonic() + seconds
    data = bytearray()
    while time.monotonic() < deadline:
        try:
            chunk = conn.receive()
        except BenchmarkError:
            break
        if chunk:
            data.extend(chunk)
            sys.stdout.write(chunk.decode("latin-1", errors="replace"))
            sys.stdout.flush()
    return bytes(data)


def ensure_shell(conn: TelnetConnection) -> None:
    for seq in (b"\x1b>>", b">\r", b"\r"):
        conn.send(seq)
        data = drain(conn, 1.0)
        if SHELL_PROMPT_RE.search(data) or SHELL_BANNER in data:
            conn.send(b"\r")
            drain(conn, 0.4)
            return
    raise RuntimeError("no shell")


def sh(conn: TelnetConnection, cmd: str, wait: float = 2.0) -> bytes:
    print(f"\n>>> {cmd}", flush=True)
    conn.send(cmd.encode("ascii") + b"\r")
    return drain(conn, wait)


def mon(conn: TelnetConnection, cmd: str, wait: float = 1.2) -> bytes:
    print(f"\n>>> mon {cmd}", flush=True)
    conn.send(cmd.encode("ascii") + b"\r")
    return drain(conn, wait)


def serial_reader(stop: threading.Event, log_path: Path, breaks: list) -> None:
    import serial

    ser = serial.Serial(COM, 115200, timeout=0.2)
    with log_path.open("wb") as f:
        buf = ""
        while not stop.is_set():
            data = ser.read(8192)
            if not data:
                continue
            f.write(data)
            f.flush()
            buf += data.decode("latin-1", errors="replace")
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                m = BREAK_RE.search(line.rstrip("\r"))
                if m and m.group(1) == BP:
                    breaks.append(
                        {
                            "r0": m.group(3),
                            "r1": m.group(4),
                            "r3": m.group(6),
                            "r4": m.group(7),
                            "r5": m.group(8),
                        }
                    )
                    print(
                        f"BREAK#{len(breaks)} R0={m.group(3)} R1={m.group(4)} "
                        f"R4={m.group(7)} R5={m.group(8)}",
                        flush=True,
                    )
    ser.close()


def parse_dump_words(text: str) -> list[str]:
    """Extract octal words from a D/M dump reply."""
    words = []
    for ln in text.splitlines():
        # "077122: 077020 047434 ..."
        if re.match(r"^[0-7]{6}:", ln.strip()):
            parts = ln.split(":", 1)[1].split()
            for p in parts:
                if re.fullmatch(r"[0-7]+", p):
                    words.append(p)
                else:
                    break
    return words


def boot_to_hang(conn: TelnetConnection) -> bytearray:
    buf = bytearray()
    for cmd, w in (
        ("rm /pdpconfig.ini", 3.0),
        ("cp /pdpconfig-211bsd.ini /pdpconfig.ini", 3.0),
        ("set pcping=0", 1.5),
    ):
        buf.extend(sh(conn, cmd, w))
    buf.extend(sh(conn, "reset", 1.0))
    time.sleep(0.3)
    buf.extend(sh(conn, "exit", 1.0))

    start = time.monotonic()
    last_out = start
    last_cr = 0.0
    cr_n = 0
    user_at = None
    while time.monotonic() < start + 200:
        now = time.monotonic()
        if cr_n < 20 and now - start < 50 and b"2.11 BSD" not in buf and now - last_cr >= 2.0:
            conn.send(b"\r")
            last_cr = now
            cr_n += 1
        try:
            chunk = conn.receive()
        except BenchmarkError:
            time.sleep(0.05)
            continue
        if chunk:
            buf.extend(chunk)
            last_out = now
            sys.stdout.write(chunk.decode("latin-1", errors="replace"))
            sys.stdout.flush()
            if b"user mem" in buf and user_at is None:
                user_at = now
                print("\n*** user mem ***\n", flush=True)
            if b"configure system" in buf or b"login: " in buf:
                break
            continue
        if user_at and now - last_out > 30:
            print("\n*** hang ***\n", flush=True)
            break
    return buf


def dump_softc(conn: TelnetConnection) -> dict:
    """Physical dump softc region; return key offsets."""
    # Dump 077122 through 077122+050 (enough for +42)
    data = mon(conn, "D077122:077172", 2.0)
    text = data.decode("latin-1", errors="replace")
    words = parse_dump_words(text)
    out = {"raw": words, "text": text}
    # word index = offset/2
    def w(off: int) -> str:
        idx = off // 2
        return words[idx] if idx < len(words) else "?"

    out["fields"] = {
        "+0": w(0),
        "+6": w(6),
        "+12": w(0o12),
        "+16": w(0o16),
        "+20": w(0o20),
        "+40": w(0o40),
        "+42": w(0o42),
    }
    return out


def main() -> int:
    OUT.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    serial_log = OUT / f"{stamp}-softc-loop-com18.log"
    telnet_log = OUT / f"{stamp}-softc-loop-telnet.log"
    breaks: list = []
    records: list = []

    stop = threading.Event()
    thr = threading.Thread(
        target=serial_reader, args=(stop, serial_log, breaks), daemon=True
    )
    thr.start()
    time.sleep(0.4)

    conn = TelnetConnection(HOST, 23, timeout=2.0)
    for attempt in range(12):
        try:
            conn.connect()
            break
        except Exception as exc:
            print("telnet", attempt, exc, flush=True)
            time.sleep(2)
            conn = TelnetConnection(HOST, 23, timeout=2.0)
    else:
        raise RuntimeError("telnet down")

    ensure_shell(conn)
    buf = boot_to_hang(conn)
    ensure_shell(conn)

    buf.extend(sh(conn, "rl regs", 2.0))
    buf.extend(sh(conn, "monitor", 0.5))
    buf.extend(mon(conn, "P", 2.0))

    print(f"\n=== B{BP}; capture softc each hit ===\n", flush=True)
    buf.extend(mon(conn, f"B{BP}", 0.5))
    buf.extend(mon(conn, "C", 0.3))

    last_n = 0
    deadline = time.monotonic() + 60.0
    while time.monotonic() < deadline and len(records) < MAX_HITS:
        t0 = time.monotonic()
        while time.monotonic() < t0 + 3.0 and len(breaks) == last_n:
            time.sleep(0.05)
        if len(breaks) == last_n:
            continue
        last_n = len(breaks)
        hit = breaks[-1]
        time.sleep(0.15)

        # Dump softc while paused
        sc = dump_softc(conn)
        print(f"\n--- hit {len(records)+1} softc ---", flush=True)
        for k, v in sc["fields"].items():
            print(f"  softc{k} = {v}", flush=True)

        # Step: MOV 20(R4),R1 ; MOV 16(R4),R0 ; COM R0 ; then ~15 more
        steps = []
        for i in range(20):
            data = mon(conn, "S", 0.4)
            buf.extend(data)
            text = data.decode("latin-1", errors="replace")
            m = STATE_RE.search(text.replace("\r", "").replace("\n", " "))
            if not m:
                # try multiline
                m = STATE_RE.search(re.sub(r"\s+", " ", text))
            if m:
                st = {
                    "pc": m.group(1),
                    "r0": m.group(2),
                    "r1": m.group(3),
                    "r2": m.group(4),
                    "r3": m.group(5),
                    "r4": m.group(6),
                    "ins": m.group(11),
                    "dis": m.group(12).strip()[:50],
                }
                # groups: 1pc 2r0 3r1 4r2 5r3 6r4 7r5 8sp 9ps 10 nextpc 11 opcode 12 dis
                st = {
                    "pc": m.group(1),
                    "r0": m.group(2),
                    "r1": m.group(3),
                    "r2": m.group(4),
                    "r3": m.group(5),
                    "r4": m.group(6),
                    "ins": m.group(11),
                    "dis": m.group(12).strip()[:50],
                }
                steps.append(st)
                print(
                    f"  S{i+1} PC={st['pc']} R0={st['r0']} R1={st['r1']} "
                    f"{st['dis']}",
                    flush=True,
                )
                # Stop once we see ffs entry or JSR to 144616 or R1 becomes 002000 again after being 0
                if st["pc"] == "144616" or "144616" in st["dis"]:
                    break
                if i >= 2 and st["r1"] == "002000" and steps[0].get("r1") != "002000":
                    print("  *** R1 reconstituted to 002000 ***", flush=True)
                    break

        records.append({"break": hit, "softc": sc["fields"], "steps": steps})

        # Continue to next loop hit
        buf.extend(mon(conn, "C", 0.3))

    buf.extend(mon(conn, "P", 1.0))
    buf.extend(mon(conn, "B clear", 0.4))
    buf.extend(mon(conn, ">", 0.4))
    buf.extend(sh(conn, "exit", 0.5))

    stop.set()
    thr.join(timeout=2)
    conn.close()
    telnet_log.write_bytes(buf)

    print("\n=== summary ===", flush=True)
    for i, rec in enumerate(records, 1):
        f = rec["softc"]
        print(
            f"#{i} +6={f['+6']} +12={f['+12']} +16={f['+16']} +20={f['+20']} "
            f"+40={f['+40']} +42={f['+42']}  BR1={rec['break']['r1']}",
            flush=True,
        )
        for st in rec["steps"][:8]:
            print(f"     {st['pc']} R0={st['r0']} R1={st['r1']} {st['dis']}", flush=True)

    print(f"\ntelnet={telnet_log}\ncom18={serial_log}", flush=True)
    return 0 if records else 1


if __name__ == "__main__":
    raise SystemExit(main())
