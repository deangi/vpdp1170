"""Extract first wake + first R4nz register dumps from becnt-simh-lite.log."""
from pathlib import Path
import sys

SRC = Path(r"C:\Users\deang\OneDrive\emulators\pdp11\rsx11mplus\becnt-simh-lite.log")
OUT = Path(__file__).resolve().parents[1] / "build" / "captures" / "becnt-simh-lite-excerpt.txt"


def find_marker(lines: list[str], needle: str, start: int = 0) -> int | None:
    for i in range(start, len(lines)):
        if needle in lines[i] and not lines[i].strip().startswith(";"):
            # prefer the echoed result line, not "sim> echo ..."
            if lines[i].startswith("===="):
                return i
    return None


def block_after(lines: list[str], idx: int, max_lines: int = 40) -> list[str]:
    """Take marker line through next blank-ish / next ==== / Breakpoint."""
    out = [lines[idx]]
    for j in range(idx + 1, min(idx + max_lines, len(lines))):
        ln = lines[j]
        if ln.startswith("====") and j > idx:
            break
        if ln.startswith("Breakpoint") and j > idx + 1:
            break
        out.append(ln)
        # stop after a clean continue following dumps
        if ln.strip() == "sim> continue" and len(out) > 8:
            break
    return out


def main() -> int:
    src = Path(sys.argv[1]) if len(sys.argv) > 1 else SRC
    if not src.is_file():
        print("missing", src)
        return 1
    lines = src.read_text(encoding="utf-8", errors="replace").splitlines()
    size = src.stat().st_size
    print(f"source: {src} ({size:,} bytes)")

    e_counts: dict[str, int] = {}
    for ln in lines:
        if ln.startswith("==== E@"):
            key = ln.strip()
            e_counts[key] = e_counts.get(key, 0) + 1
    print("marker counts:")
    for k, v in sorted(e_counts.items()):
        print(f"  {v:5d}  {k}")

    wanted = [
        "==== E@005172 ====",
        "==== E@005474 ====",
        "==== E@006420 ====",
        "==== E@006454 ====",
        "==== E@007040 ====",
        "==== E@007054 abort ====",
    ]
    chunks: list[str] = [
        f"; excerpt from {src.name}",
        "; FIRST wake chain (first occurrence of each marker)",
        "",
    ]
    pos = 0
    first_5172 = find_marker(lines, "==== E@005172 ====")
    if first_5172 is None:
        print("no E@005172")
        return 2
    pos = first_5172
    for needle in wanted:
        i = find_marker(lines, needle, pos)
        if i is None:
            chunks.append(f"; MISSING {needle}")
            continue
        chunks.append(f"; --- line {i + 1} ---")
        chunks.extend(block_after(lines, i))
        chunks.append("")
        pos = i + 1

    # first R4nz anywhere
    r4 = find_marker(lines, "==== E@006464 R4nz ====")
    chunks.append("; FIRST R4nz (successful install path)")
    chunks.append("")
    if r4 is None:
        chunks.append("; MISSING E@006464")
    else:
        chunks.append(f"; --- line {r4 + 1} ---")
        chunks.extend(block_after(lines, r4, 50))
        # also grab surrounding 6420/6454 if just before
        for back in ("==== E@006454 ====", "==== E@006420 ====", "==== E@005172 ===="):
            bi = None
            for i in range(r4, max(r4 - 200, 0), -1):
                if lines[i].startswith(back):
                    bi = i
                    break
            if bi is not None:
                chunks.insert(-len(block_after(lines, r4, 50)) - 2, f"; pre-R4nz {back} @ line {bi + 1}")
                # keep simple: append context at end instead
        chunks.append("")
        # dump 5172/6420/6454 immediately before this R4nz
        ctx_start = max(0, r4 - 120)
        pre = []
        for i in range(ctx_start, r4):
            if lines[i].startswith("==== E@"):
                pre.append(i)
        # last few markers before R4nz
        for i in pre[-4:]:
            chunks.append(f"; --- line {i + 1} (pre-R4nz) ---")
            chunks.extend(block_after(lines, i))
            chunks.append("")
        chunks.append(f"; --- line {r4 + 1} (R4nz) ---")
        chunks.extend(block_after(lines, r4, 50))

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(chunks) + "\n", encoding="utf-8")
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
