#!/usr/bin/env python3
"""Build a small CJK TTF subset for an App UI.

Example:
  python make_cjk_subset.py --source-font NotoSerifSC-Medium.ttf \
    --lua assets/examples/sd_fallback/app/scripts/main.lua \
    --output assets/examples/sd_fallback/app/assets/ui_cn.ttf
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path


def collect_chars(paths: list[Path], include_ascii: bool) -> str:
    chars: set[str] = set()
    for path in paths:
        chars |= set(path.read_text(encoding="utf-8", errors="replace"))
    if include_ascii:
        chars |= {chr(c) for c in range(0x20, 0x7F)}
    for ch in ("\r", "\n", "\t"):
        chars.discard(ch)
    return "".join(sorted(chars))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-font", required=True, type=Path,
                        help="Full TTF/OTF source font")
    parser.add_argument("--lua", action="append", default=[], type=Path,
                        help="Lua source file to scan (repeatable)")
    parser.add_argument("--text-file", action="append", default=[], type=Path,
                        help="UTF-8 text file to scan (repeatable)")
    parser.add_argument("--output", required=True, type=Path, help="Output TTF")
    parser.add_argument("--include-ascii", action="store_true", default=True,
                        help="Include printable ASCII (default)")
    parser.add_argument("--no-ascii", dest="include_ascii", action="store_false")
    parser.add_argument("--max-kb", type=int, default=200,
                        help="Warn above this output size (default 200)")
    args = parser.parse_args()

    try:
        from fontTools import subset
        from fontTools.ttLib import TTFont
    except ImportError:
        print("error: fontTools is required; run: pip install fonttools", file=sys.stderr)
        return 3

    if not args.source_font.is_file():
        print(f"error: source font not found: {args.source_font}", file=sys.stderr)
        return 2
    files = list(args.lua) + list(args.text_file)
    for path in files:
        if not path.is_file():
            print(f"error: text file not found: {path}", file=sys.stderr)
            return 2
    text = collect_chars(files, args.include_ascii)
    if not text:
        print("error: no text collected", file=sys.stderr)
        return 2

    options = subset.Options()
    options.layout_features = ["*"]
    options.name_IDs = ["*"]
    options.notdef_outline = True
    options.recalc_bounds = True
    font = subset.load_font(str(args.source_font), options)
    subsetter = subset.Subsetter(options=options)
    subsetter.populate(text=text)
    subsetter.subset(font)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    subset.save_font(font, str(args.output), options)
    font.close()

    check = TTFont(str(args.output))
    cmap = check.getBestCmap()
    missing = sorted({c for c in text if ord(c) not in cmap})
    size = args.output.stat().st_size
    print(f"wrote {args.output} ({size} bytes, {len(text)} chars)")
    if missing:
        print("missing: " + " ".join(f"U+{ord(c):04X}" for c in missing), file=sys.stderr)
        return 1
    if size > args.max_kb * 1024:
        print(f"warning: subset is larger than {args.max_kb} KiB; trim copy", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
