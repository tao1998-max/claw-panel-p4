#!/usr/bin/env python3
"""Build the two Noto subsets used by apps/morse_station.

  ui_cn.ttf   Noto Sans SC, weight 560: every character in main.lua + ASCII.
  display.ttf Noto Serif SC, weight 650: the big letters / title glyphs.

Source files are the Windows Noto variable fonts.  Requires fontTools.
"""

from __future__ import annotations

import sys
from pathlib import Path

try:
    from fontTools import subset
    from fontTools.ttLib import TTFont
    from fontTools.varLib.instancer import instantiateVariableFont
except ImportError:
    sys.exit("fontTools is required: pip install fonttools")

ROOT = Path(__file__).resolve().parent.parent
APP = ROOT / "apps" / "morse_station"
LUA = APP / "scripts" / "main.lua"
OUT = APP / "assets"
SANS = Path(r"C:\Windows\Fonts\NotoSansSC-VF.ttf")
SERIF = Path(r"C:\Windows\Fonts\NotoSerifSC-VF.ttf")

DISPLAY_CHARS = set(
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "0123456789"
    " ?·−×"
    "电码站发报抄收文中断"
)


def collect_lua_chars(path: Path) -> set[str]:
    chars = set(path.read_text(encoding="utf-8", errors="replace"))
    chars |= {chr(c) for c in range(0x20, 0x7F)}
    for ch in ("\r", "\n", "\t"):
        chars.discard(ch)
    return chars


def build(src: Path, weight: int, chars: set[str], dest: Path, label: str) -> None:
    options = subset.Options()
    options.layout_features = ["*"]
    options.name_IDs = ["*"]
    options.notdef_outline = True
    options.recalc_bounds = True

    font = subset.load_font(str(src), options)
    if "fvar" in font:
        instantiateVariableFont(font, {"wght": weight}, inplace=True)
    subsetter = subset.Subsetter(options=options)
    subsetter.populate(text="".join(sorted(chars)))
    subsetter.subset(font)
    dest.parent.mkdir(parents=True, exist_ok=True)
    subset.save_font(font, str(dest), options)
    font.close()

    check = TTFont(str(dest))
    cmap = check.getBestCmap()
    missing = sorted(c for c in chars if ord(c) not in cmap)
    size = dest.stat().st_size
    print(f"{label}: {dest}  {size} bytes  {len(chars)} chars")
    if missing:
        print("  missing:", " ".join(f"U+{ord(c):04X}" for c in missing))
        raise SystemExit(1)
    if size > 200 * 1024:
        print(f"  warning: {label} is larger than 200 KiB")


def main() -> int:
    if not LUA.is_file():
        sys.exit(f"missing {LUA}")
    ui_chars = collect_lua_chars(LUA)
    build(SANS, 560, ui_chars, OUT / "ui_cn.ttf", "UI Sans SC 560")
    build(SERIF, 650, DISPLAY_CHARS, OUT / "display.ttf", "Display Serif SC 650")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
