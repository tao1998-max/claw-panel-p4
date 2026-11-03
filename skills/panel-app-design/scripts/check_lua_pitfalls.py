#!/usr/bin/env python3
"""Statically check App Lua for the pitfalls that make it exit right after launch.

Checks a package (or any .lua files/directories) with plain regexes, so it needs
no Lua interpreter:

1. ERROR  Method or field named after a Lua keyword (`u:local()`, `t.end`).
   The binding cannot expose these; Lua rejects them at compile time.
2. ERROR  `:on("click", ...)`; the LVGL binding only knows "clicked".
3. WARN   Single `/` in an LVGL integer option (`y = H / 2` is a float).
4. WARN   `cond and f() or g()` when f/g return several values; and/or keeps
   only the first one, which silently drops colours or other secondary returns.

Exit code: 0 when clean, 1 on errors (warnings fail only with --strict).
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

KEYWORDS = {
    "and", "break", "do", "else", "elseif", "end", "false", "for", "function",
    "global", "goto", "if", "in", "local", "nil", "not", "or", "repeat",
    "return", "then", "true", "until", "while",
}

INT_OPTIONS = (
    "x", "y", "w", "h", "pad", "radius", "border_width", "line_width",
    "pad_row", "pad_column", "arc_width",
)

RE_KEYWORD_METHOD = re.compile(
    r"[:.](" + "|".join(sorted(KEYWORDS)) + r")\s*\(")
RE_CLICK = re.compile(r':on\(\s*"click"\s*[,)]')
RE_INT_OPTION_DIV = re.compile(
    r"\b(" + "|".join(INT_OPTIONS) + r")\s*=\s*[^,\n]*?(?<!/)/(?!/)")
RE_AND_OR_CALL = re.compile(r"\band\s+\w+\([^\n;]*?\)\s+or\s+\w+\(")


def strip_comments_and_strings(line: str) -> str:
    """Remove -- comments and simple quoted strings so patterns hit code only."""
    out = []
    i = 0
    quote = None
    while i < len(line):
        ch = line[i]
        if quote:
            if ch == "\\":
                i += 2
                continue
            if ch == quote:
                quote = None
            i += 1
            continue
        if ch in ('"', "'"):
            quote = ch
            out.append(" ")
            i += 1
            continue
        if ch == "-" and line[i:i + 2] == "--":
            break
        out.append(ch)
        i += 1
    return "".join(out)


def check_text(path: Path, text: str) -> list[dict]:
    issues: list[dict] = []
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = strip_comments_and_strings(raw)
        if not line.strip():
            continue

        m = RE_KEYWORD_METHOD.search(line)
        if m:
            issues.append({
                "severity": "error",
                "line": lineno,
                "message": f"'.{m.group(1)}(' uses a Lua keyword as a method/field name",
                "hint": "rename the method (for example local -> local_addr)",
            })

        if RE_CLICK.search(raw):
            issues.append({
                "severity": "error",
                "line": lineno,
                "message": 'event name "click" is not supported by the LVGL binding',
                "hint": 'use "clicked"',
            })

        m = RE_INT_OPTION_DIV.search(line)
        if m:
            prefix = line[:m.start()]
            # `local y = ...` and `for y = ...` are ordinary variables/loops,
            # not LVGL option-table fields.
            inside_table = prefix.count("{") > prefix.count("}")
            if not inside_table and (("local" in prefix) or ("for" in prefix)):
                m = None
        if m:
            issues.append({
                "severity": "warning",
                "line": lineno,
                "message": f"option '{m.group(1)}' uses / (Lua 5.4+ returns a float)",
                "hint": "use // or math.floor(); LVGL integer options reject floats",
            })

        m = RE_AND_OR_CALL.search(line)
        if m:
            issues.append({
                "severity": "warning",
                "line": lineno,
                "message": "and/or around a call keeps only the first return value",
                "hint": "use explicit if/else returns (or store the values first)",
            })

    for issue in issues:
        issue["file"] = str(path)
    return issues


def collect_lua_paths(targets: list[str]) -> list[Path]:
    files: list[Path] = []
    for target in targets:
        path = Path(target)
        if path.is_dir():
            files.extend(sorted(path.rglob("*.lua")))
        elif path.is_file():
            files.append(path)
        else:
            print(f"warning: not found: {path}", file=sys.stderr)
    return [p for p in files if p.suffix == ".lua"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="+",
                        help="App package, script directory, or .lua files")
    parser.add_argument("--strict", action="store_true",
                        help="Treat warnings as failures")
    parser.add_argument("--json", action="store_true", help="Emit JSON")
    args = parser.parse_args()

    files = collect_lua_paths(args.paths)
    if not files:
        print("error: no .lua files found", file=sys.stderr)
        return 2

    issues: list[dict] = []
    for path in files:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            print(f"warning: cannot read {path}: {exc}", file=sys.stderr)
            continue
        issues.extend(check_text(path, text))

    errors = [i for i in issues if i["severity"] == "error"]
    warnings = [i for i in issues if i["severity"] == "warning"]

    if args.json:
        print(json.dumps({"files": len(files), "issues": issues},
                         ensure_ascii=False, indent=2))
    else:
        for issue in issues:
            print(f"{issue['file']}:{issue['line']}: "
                  f"{issue['severity'].upper()}: {issue['message']}")
            print(f"    hint: {issue['hint']}")
        print(f"checked {len(files)} file(s): {len(errors)} error(s), "
              f"{len(warnings)} warning(s)")

    if errors or (args.strict and warnings):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
