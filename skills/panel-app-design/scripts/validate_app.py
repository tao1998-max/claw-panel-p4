#!/usr/bin/env python3
"""Validate a launcher App package and its optional SD fallback manifest."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

from _common import (FAT_CLUSTER, fail, human_bytes, is_relative_inside,
                     jpeg_dimensions, package_bytes, package_bytes_rounded,
                     safe_join)

ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,63}$")
KINDS = {"jpeg", "rgb565", "argb8888", "eaf", "ogg"}
SD_TARGET_KB = 300
SD_WARN_KB = 512
SD_HARD_KB = 1024


class Report:
    def __init__(self):
        self.issues: list[dict[str, str]] = []

    def add(self, level: str, message: str):
        self.issues.append({"level": level, "message": message})

    def error(self, message: str):
        self.add("error", message)

    def warn(self, message: str):
        self.add("warning", message)

    def ok(self, message: str):
        self.add("ok", message)

    def has_errors(self) -> bool:
        return any(i["level"] == "error" for i in self.issues)

    def print(self):
        for issue in self.issues:
            print(f"[{issue['level']}] {issue['message']}")


def read_json(path: Path, report: Report):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        report.error(f"missing file: {path}")
    except json.JSONDecodeError as exc:
        report.error(f"invalid JSON in {path}: {exc}")
    return None


def jpeg_info(path: Path):
    """Return (kind, width, height) for a JPEG file, or None."""
    try:
        data = path.read_bytes()
    except OSError:
        return None
    if len(data) < 4 or data[0] != 0xFF or data[1] != 0xD8:
        return None
    i = 2
    while i + 4 <= len(data):
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        if marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
            i += 2
            continue
        length = int.from_bytes(data[i + 2:i + 4], "big")
        if marker in (0xC0, 0xC1, 0xC2):
            height = int.from_bytes(data[i + 5:i + 7], "big")
            width = int.from_bytes(data[i + 7:i + 9], "big")
            kind = "baseline" if marker == 0xC0 else ("extended" if marker == 0xC1 else "progressive")
            return kind, width, height
        i += 2 + length
    return None


def validate_launcher(app_dir: Path, report: Report, expected_id: str):
    manifest_path = app_dir / "launcher.json"
    if not manifest_path.is_file():
        report.error("launcher.json is missing")
        return None
    size = manifest_path.stat().st_size
    if size > 4096:
        report.error(f"launcher.json is {size} bytes (limit 4096)")
    data = read_json(manifest_path, report)
    if data is None:
        return None

    if data.get("schema_version") != 1:
        report.error("launcher.json schema_version must be 1")
    app_id = data.get("id")
    if not isinstance(app_id, str) or not ID_RE.match(app_id):
        report.error("launcher.json id must match [A-Za-z0-9_-]{1,63}")
    elif app_id != expected_id:
        report.error(f"launcher id {app_id!r} does not match deployed directory {expected_id!r}")
    entry = data.get("entry")
    if not isinstance(entry, str) or not entry.endswith(".lua") or not is_relative_inside(entry):
        report.error("launcher entry must be a package-relative .lua path")
    else:
        entry_path = safe_join(app_dir, entry)
        if not entry_path.is_file():
            report.error(f"launcher entry does not exist: {entry}")
        elif entry_path.stat().st_size > 64 * 1024:
            report.error(f"entry Lua is {entry_path.stat().st_size} bytes (limit 64 KiB)")

    icon = data.get("icon")
    if icon is None:
        report.warn("no launcher icon; the home page will show a generated fallback")
    elif not isinstance(icon, str) or not icon.lower().endswith((".jpg", ".jpeg")) or not is_relative_inside(icon):
        report.error("launcher icon must be a package-relative .jpg/.jpeg path")
    else:
        icon_path = safe_join(app_dir, icon)
        if not icon_path.is_file():
            report.warn(f"launcher icon is referenced but missing: {icon}")
        else:
            info = jpeg_info(icon_path)
            if info is None:
                report.error(f"launcher icon is not a readable JPEG: {icon}")
            else:
                kind, width, height = info
                if kind == "progressive":
                    report.error(f"launcher icon must be baseline JPEG (progressive rejected): {icon}")
                if (width, height) != (96, 96):
                    report.warn(f"launcher icon is {width}x{height}; 96x96 is the home grid size")
    version = data.get("version")
    if version is None:
        report.warn("no launcher version (recommended: MAJOR.MINOR.PATCH)")
    elif not isinstance(version, str) or not version or len(version) > 31 or             any(ch not in "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._+-" for ch in version):
        report.error("launcher version must be 1-31 chars of [0-9A-Za-z._+-]")
    elif not re.match(r"^\d+\.\d+\.\d+$", version):
        report.warn(f"launcher version {version!r} is not MAJOR.MINOR.PATCH")
    if "args" in data and not isinstance(data["args"], dict):
        report.error("launcher args must be an object")
    if "order" in data and not isinstance(data["order"], int):
        report.error("launcher order must be an integer")
    if "visible" in data and not isinstance(data["visible"], bool):
        report.error("launcher visible must be a boolean")
    return data


def validate_sd_manifest(app_dir: Path, app_id: str, sd_dir: Path | None, report: Report):
    path = app_dir / "sd_manifest.json"
    if not path.is_file():
        report.ok("no sd_manifest.json (FAT-only app)")
        return
    data = read_json(path, report)
    if data is None:
        return
    if data.get("schema_version") != 1:
        report.error("sd_manifest.json schema_version must be 1")
    if data.get("app_id") != app_id:
        report.error("sd_manifest.json app_id must match launcher id")
    assets = data.get("assets")
    if not isinstance(assets, list):
        report.error("sd_manifest.json assets must be an array")
        return

    for index, asset in enumerate(assets):
        where = f"sd_manifest asset[{index}]"
        if not isinstance(asset, dict):
            report.error(f"{where} must be an object")
            continue
        sd_rel = asset.get("sd")
        kind = asset.get("kind")
        if not is_relative_inside(sd_rel):
            report.error(f"{where} sd must be a relative path")
            continue
        if kind not in KINDS:
            report.error(f"{where} kind must be one of {sorted(KINDS)}")
        if sd_rel.lower().endswith((".ttf", ".lua", ".json")):
            report.error(f"{where} must not put executable/font files on SD")
        width = asset.get("width")
        height = asset.get("height")
        if kind in ("jpeg", "rgb565", "argb8888"):
            if not isinstance(width, int) or not isinstance(height, int) or width <= 0 or height <= 0:
                report.error(f"{where} needs positive integer width/height")
        optional = bool(asset.get("optional", False))
        fallback = asset.get("fallback")
        if fallback is not None:
            if not is_relative_inside(fallback):
                report.error(f"{where} fallback must be package-relative")
            else:
                fallback_path = safe_join(app_dir, fallback)
                if not fallback_path.is_file():
                    report.error(f"{where} fallback is missing: {fallback}")
                elif kind in ("jpeg", "argb8888", "rgb565") and isinstance(width, int) and isinstance(height, int):
                    dims = jpeg_dimensions(fallback_path) if fallback.lower().endswith((".jpg", ".jpeg")) else None
                    if dims and dims != (width, height):
                        report.error(f"{where} fallback is {dims[0]}x{dims[1]}, expected {width}x{height}")
        elif not optional:
            report.error(f"{where} needs a fallback or optional=true")

        if sd_dir is not None:
            local = sd_dir / "panel-assets" / app_id / sd_rel
            if not local.is_file():
                report.error(f"{where} SD file is missing: {local}")
            else:
                expected = asset.get("bytes")
                if isinstance(expected, int) and local.stat().st_size != expected:
                    report.error(f"{where} bytes={expected} but file is {local.stat().st_size}")
        report.ok(f"{where} ok: {sd_rel}")


def validate_lua_scan(app_dir: Path, report: Report):
    for script in sorted(app_dir.rglob("*.lua")):
        text = script.read_text(encoding="utf-8", errors="replace")
        if re.search(r"font_load[^\n]*['\"]/sdcard", text):
            report.error(f"{script.name}: lvgl.font_load must not read /sdcard")
        if re.search(r"lvgl\.image\s*\([^\n]*src\s*=\s*['\"]/sdcard", text):
            report.error(f"{script.name}: lvgl.image src must not read /sdcard")
        if re.search(r"lvgl\.eaf\s*\([^\n]*src\s*=\s*['\"]/sdcard", text):
            report.error(f"{script.name}: lvgl.eaf src must not read /sdcard; use src_data")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app-dir", required=True, type=Path, help="App package directory")
    parser.add_argument("--sd-dir", type=Path, help="Local SD tree (contains panel-assets/<id>/...)")
    parser.add_argument("--fat-free-kb", type=int, help="Current FAT free space in KiB")
    parser.add_argument("--allow-large", action="store_true", help="Allow packages larger than 1 MiB")
    parser.add_argument("--remote-id", help="Deployed package directory name (defaults to app dir name)")
    parser.add_argument("--json", action="store_true", help="Emit JSON report")
    args = parser.parse_args()

    app_dir = args.app_dir.resolve()
    if not app_dir.is_dir():
        fail(f"not a directory: {app_dir}")

    report = Report()
    launcher = validate_launcher(app_dir, report, args.remote_id or app_dir.name)
    app_id = launcher.get("id") if isinstance(launcher, dict) else (args.remote_id or app_dir.name)
    if isinstance(app_id, str):
        validate_sd_manifest(app_dir, app_id, args.sd_dir, report)
    validate_lua_scan(app_dir, report)

    raw = package_bytes(app_dir)
    rounded = package_bytes_rounded(app_dir)
    report.ok(f"package raw={human_bytes(raw)} cluster-rounded={human_bytes(rounded)}")
    if rounded > SD_TARGET_KB * 1024:
        report.warn(f"package exceeds the {SD_TARGET_KB} KiB target")
    if rounded > SD_WARN_KB * 1024:
        report.warn(f"package exceeds {SD_WARN_KB} KiB; move heavy assets to SD")
    if rounded > SD_HARD_KB * 1024 and not args.allow_large:
        report.error(f"package exceeds the {SD_HARD_KB} KiB hard limit (use --allow-large)")
    if args.fat_free_kb is not None:
        reserve = 64 * 1024
        if rounded > args.fat_free_kb * 1024 - reserve:
            report.error(f"package does not fit FAT free space with 64 KiB reserve")

    if args.json:
        print(json.dumps({"app_dir": str(app_dir), "issues": report.issues,
                          "raw_bytes": raw, "rounded_bytes": rounded}, indent=2))
    else:
        report.print()
    return 1 if report.has_errors() else 0


if __name__ == "__main__":
    sys.exit(main())
