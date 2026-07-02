# TODO

## VT100 Screen Processor Updates

Port the VT100 parsing improvements proven in `pyTelnetClient` into the
ESP32 TFT screen processor (`console.cpp`):

- Parse DEC private CSI sequences with `?`, especially `ESC[?8h` and
  `ESC[?8l` (`DECARM`). These should be consumed cleanly even if the local
  emulator does not implement keyboard auto-repeat behavior.
- Preserve existing handling for cursor/home/clear sequences seen from the
  PDP software: `ESC[H`, `ESC[J`, `ESC[15H`, and related CSI cursor movement
  and erase commands.
- Implement VT100 scrolling regions:
  - `ESC[<top>;<bottom>r` (`DECSTBM`) sets top/bottom scroll margins.
  - `ESC[;22r` means rows 1 through 22 scroll, leaving rows below the region
    fixed.
  - Line feed and auto-wrap at the bottom margin must scroll only inside the
    active region.
  - Reverse index (`ESC M`) at the top margin must reverse-scroll the active
    region down, inserting a blank line at the top and leaving rows outside
    the margins fixed.
  - Index (`ESC D`) at the bottom margin must scroll the active region up.
  - `ESC[r` resets the region to the full screen.
- Implement DEC origin mode (`ESC[?6h` / `ESC[?6l`) enough that cursor
  addressing is relative to the active scroll region when enabled and
  absolute when disabled.
- Implement SGR inverse video:
  - `ESC[7m` turns inverse video on for subsequently written cells.
  - `ESC[m` / `ESC[0m` resets attributes.
  - Store inverse-video state per screen cell so redraw/scroll/erase preserve
    the correct foreground/background relationship.
- Implement VT100 character-set designation and line drawing:
  - `ESC(0` / `ESC)0` select the VT100 special graphics set for G0/G1.
  - `ESC(B` / `ESC)B` restore ASCII for G0/G1.
  - Support `SO` (`0x0E`) to invoke G1 and `SI` (`0x0F`) to invoke G0.
  - Translate special graphics bytes such as `q`, `x`, `l`, `k`, `m`, `j`,
    `t`, `u`, `v`, `w`, and `n` into the display's line-drawing glyphs.
  - Keep G1 designation separate from invocation: `ESC)0` only designates
    G1 as special graphics; it must not render lowercase bytes through G1
    until `SO` (`0x0E`) invokes G1. `SI` (`0x0F`) must return rendering to G0.
- Consume VT100 character-set designation sequences (`ESC)B`, `ESC(B`, etc.)
  without leaking the final designator byte onto the screen.
- Add focused parser tests or a small debug harness case for:
  - `ESC[?8l` and `ESC[?8h`
  - `ESC[7m...ESC[m`
  - `ESC(0lqkESC(B`
  - `ESC)0qqq` remains lowercase in G0
  - `ESC)0 SO qqq SI q` renders the middle `q`s as line drawing and the
    final `q` as lowercase G0 text
