"""Extract first post-banner switcher snapshot from becnt-simh.log."""
from pathlib import Path
import re
import sys

SRC = Path(r"C:\Users\deang\OneDrive\emulators\pdp11\rsx11mplus\becnt-simh.log")
OUT = Path(__file__).resolve().parents[1] / "build" / "captures" / "becnt-simh-firstwake.txt"

MARKERS = (
    "==== E@005172",
    "==== E@005474",
    "==== E@006420",
    "==== E@006454",
    "==== E@006464",
    "==== E@007040",
    "==== E@007054",
)


def main() -> int:
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else SRC
    if not src.is_file():
        print("missing", src)
        return 1
    size = src.stat().st_size
    print(f"source: {src} ({size:,} bytes, {size/1024/1024:.1f} MB)")

    banner = None
    first_5172 = None
    lines = src.read_text(encoding="utf-8", errors="replace").splitlines()
    for i, line in enumerate(lines):
        if "RSX-11M-PLUS" in line and banner is None:
            banner = i
        if "==== E@005172" in line and first_5172 is None and banner is not None:
            first_5172 = i
            break

    if first_5172 is None:
        print("no post-banner E@005172 found")
        return 2

    end = first_5172
    for j in range(first_5172, min(first_5172 + 400, len(lines))):
        if "==== E@007054" in lines[j]:
            end = j
        if end > first_5172 and lines[j].strip() == "sim> continue" and j > end + 5:
            end = j
            break

    chunk = lines[first_5172 : end + 1]
    OUT.parent.mkdir(parents=True, exist_ok=True)
    header = [
        f"; extracted from {src.name}",
        f"; banner line {banner + 1}, first wake E@005172 line {first_5172 + 1}",
        "",
    ]
    OUT.write_text("\n".join(header + chunk) + "\n", encoding="utf-8")
    print(f"wrote {OUT} ({len(chunk)} lines)")
  # counts
    e = sum(1 for ln in lines if "==== E@" in ln)
    w = sum(1 for ln in lines if "==== W@" in ln)
    print(f"log had {e:,} E@ markers and {w:,} W@ markers")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
