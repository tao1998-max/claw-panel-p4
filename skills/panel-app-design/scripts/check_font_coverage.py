#!/usr/bin/env python3
"""Check whether a TTF covers every character used by a Lua UI."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def collect_text(paths: list[Path], extra: str = "") -> set[str]:
    chars: set[str] = set(extra)
    for path in paths:
        chars |= set(path.read_text(encoding="utf-8", errors="replace"))
    chars |= {chr(c) for c in range(0x20, 0x7F)}
    chars.discard("\r")
    chars.discard("\n")
    chars.discard("\t")
    return chars


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ttf", required=True, type=Path, help="TTF to inspect")
    parser.add_argument("--lua", action="append", default=[], type=Path,
                        help="Lua source file (repeatable)")
    parser.add_argument("--text-file", action="append", default=[], type=Path,
                        help="UTF-8 text file (repeatable)")
    parser.add_argument("--text", default="", help="Extra characters to check")
    parser.add_argument("--json", action="store_true", help="Emit JSON")
    args = parser.parse_args()

    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        print("error: fontTools is required; run: pip install fonttools", file=sys.stderr)
        return 3

    if not args.ttf.is_file():
        print(f"error: TTF not found: {args.ttf}", file=sys.stderr)
        return 2
    files = list(args.lua) + list(args.text_file)
    for path in files:
        if not path.is_file():
            print(f"error: text file not found: {path}", file=sys.stderr)
            return 2

    text = collect_text(files, args.text)
    font = TTFont(str(args.ttf))
    cmap = font.getBestCmap()
    missing = sorted({c for c in text if ord(c) not in cmap})
    if args.json:
        print(json.dumps({"ttf": str(args.ttf), "checked": len(text),
                          "missing": missing}, ensure_ascii=False, indent=2))
    else:
        print(f"checked {len(text)} characters in {args.ttf}")
        if missing:
            print("missing glyphs: " + " ".join(f"{c}(U+{ord(c):04X})" for c in missing))
        else:
            print("all characters covered")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
