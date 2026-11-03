#!/usr/bin/env python3
"""Prepare SD assets and same-size FAT fallbacks for a panel App.

Reads a JSON spec. Raster sources are converted to baseline JPEG or tightly
packed raw pixels; EAF/OGG files are copied. The generated sd_manifest.json is
tooling-only: the App does not parse it at runtime.
"""

from __future__ import annotations

import argparse
import json
import shutil
import sys
from pathlib import Path

from _common import fail, human_bytes, is_relative_inside, safe_join, write_json

KINDS = {"jpeg", "rgb565", "argb8888", "eaf", "ogg"}

# 8x8 ordered Bayer matrix for RGB565 quantization. Optional per-asset
# ("dither": true) because it removes banding in smooth dark gradients.
BAYER8 = (
    (0, 32, 8, 40, 2, 34, 10, 42),
    (48, 16, 56, 24, 50, 18, 58, 26),
    (12, 44, 4, 36, 14, 46, 6, 38),
    (60, 28, 52, 20, 62, 30, 54, 22),
    (3, 35, 11, 43, 1, 33, 9, 41),
    (51, 19, 59, 27, 49, 17, 57, 25),
    (15, 47, 7, 39, 13, 45, 5, 37),
    (63, 31, 55, 23, 61, 29, 53, 21),
)


def bayer_quantize(value: int, bits: int, x: int, y: int) -> int:
    levels = (1 << bits) - 1
    threshold = (BAYER8[y & 7][x & 7] + 0.5) / 64.0 - 0.5
    q = int(value / 255.0 * levels + 0.5 + threshold)
    return 0 if q < 0 else (levels if q > levels else q)


def load_spec(path: Path):
    try:
        spec = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        fail(f"spec not found: {path}")
    except json.JSONDecodeError as exc:
        fail(f"invalid spec JSON: {exc}")
    app_id = spec.get("app_id")
    if not isinstance(app_id, str) or not app_id:
        fail("spec.app_id is required")
    assets = spec.get("assets")
    if not isinstance(assets, list) or not assets:
        fail("spec.assets must be a non-empty array")
    return spec


def relative_to(base: Path, value: str) -> Path:
    path = Path(value)
    return path if path.is_absolute() else (base / path)


def resize_cover(image, width: int, height: int):
    from PIL import ImageOps
    return ImageOps.fit(image.convert("RGB"), (width, height), method=0)


def write_jpeg(image, path: Path, quality: int):
    path.parent.mkdir(parents=True, exist_ok=True)
    image.convert("RGB").save(path, "JPEG", quality=quality, subsampling=2,
                              optimize=True, progressive=False)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spec", required=True, type=Path, help="JSON asset plan")
    parser.add_argument("--package-dir", type=Path,
                        help="App package directory (default: <spec-dir>/app)")
    parser.add_argument("--sd-root", type=Path,
                        help="SD root or staging root (default: <spec-dir>/sd)")
    parser.add_argument("--no-fallbacks", action="store_true",
                        help="Do not generate package fallback images")
    parser.add_argument("--no-manifest", action="store_true",
                        help="Do not write package sd_manifest.json")
    parser.add_argument("--dry-run", action="store_true", help="Print plan only")
    args = parser.parse_args()

    try:
        from PIL import Image
    except ImportError:
        print("error: Pillow is required; run: pip install pillow", file=sys.stderr)
        return 3

    spec_path = args.spec.resolve()
    spec_dir = spec_path.parent
    spec = load_spec(spec_path)
    app_id = spec["app_id"]
    package_dir = (args.package_dir or (spec_dir / "app")).resolve()
    sd_root = (args.sd_root or (spec_dir / "sd")).resolve()
    sd_app_dir = sd_root / "panel-assets" / app_id
    manifest_assets = []

    if not package_dir.is_dir():
        fail(f"package dir not found: {package_dir}")

    for index, asset in enumerate(spec["assets"]):
        where = f"assets[{index}]"
        if not isinstance(asset, dict):
            fail(f"{where} must be an object")
        source = relative_to(spec_dir, asset.get("source", ""))
        sd_rel = asset.get("sd")
        kind = asset.get("kind")
        if not source.is_file():
            fail(f"{where} source not found: {source}")
        if not is_relative_inside(sd_rel):
            fail(f"{where} sd must be a relative path")
        if kind not in KINDS:
            fail(f"{where} kind must be one of {sorted(KINDS)}")
        if sd_rel.lower().endswith((".ttf", ".lua", ".json")):
            fail(f"{where} refuses executable/font/json files on SD")

        sd_out = safe_join(sd_app_dir, sd_rel)
        entry = {
            "role": asset.get("role", ""),
            "layer": asset.get("layer", ""),
            "sd": sd_rel,
            "kind": kind,
        }
        if kind in ("jpeg", "rgb565", "argb8888"):
            width = int(asset.get("width", 0))
            height = int(asset.get("height", 0))
            if width <= 0 or height <= 0:
                fail(f"{where} width/height must be positive")
            entry["width"] = width
            entry["height"] = height

        fallback_rel = asset.get("fallback")
        fallback_path = None
        if fallback_rel is not None:
            if not is_relative_inside(fallback_rel):
                fail(f"{where} fallback must be package-relative")
            fallback_path = safe_join(package_dir, fallback_rel)
            entry["fallback"] = fallback_rel

        if args.dry_run:
            print(f"{where}: {source} -> {sd_out} ({kind})")
            continue

        sd_out.parent.mkdir(parents=True, exist_ok=True)

        if kind == "jpeg":
            image = Image.open(source)
            full = resize_cover(image, entry["width"], entry["height"])
            quality = int(asset.get("quality", 88))
            write_jpeg(full, sd_out, quality)
            if fallback_path is not None and not args.no_fallbacks:
                write_jpeg(full, fallback_path, int(asset.get("fallback_quality", 50)))
                entry["fallback_bytes"] = fallback_path.stat().st_size
        elif kind == "rgb565":
            image = resize_cover(Image.open(source), entry["width"], entry["height"])
            raw = image.tobytes("raw", "RGB")
            dither = bool(asset.get("dither", False))
            width, height = entry["width"], entry["height"]
            out = bytearray()
            for y in range(height):
                for x in range(width):
                    i = (y * width + x) * 3
                    r, g, b = raw[i], raw[i + 1], raw[i + 2]
                    if dither:
                        value = ((bayer_quantize(r, 5, x, y) << 11) |
                                 (bayer_quantize(g, 6, x, y) << 5) |
                                 bayer_quantize(b, 5, x, y))
                    else:
                        value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                    out += bytes((value & 0xFF, (value >> 8) & 0xFF))
            sd_out.write_bytes(out)
            if fallback_path is not None and not args.no_fallbacks:
                write_jpeg(image, fallback_path, int(asset.get("fallback_quality", 60)))
                entry["fallback_bytes"] = fallback_path.stat().st_size
        elif kind == "argb8888":
            image = Image.open(source).convert("RGBA")
            fitted = image.resize((entry["width"], entry["height"]))
            raw = fitted.tobytes("raw", "RGBA")
            out = bytearray()
            for i in range(0, len(raw), 4):
                r, g, b, a = raw[i], raw[i + 1], raw[i + 2], raw[i + 3]
                out += bytes((b, g, r, a))
            sd_out.write_bytes(out)
            if fallback_path is not None and not args.no_fallbacks:
                background = asset.get("fallback_background", "#090d14")
                bg = Image.new("RGB", fitted.size, background)
                bg.paste(fitted, (0, 0), fitted)
                write_jpeg(bg, fallback_path, int(asset.get("fallback_quality", 80)))
                entry["fallback_bytes"] = fallback_path.stat().st_size
        elif kind in ("eaf", "ogg"):
            if source.suffix.lower() != "." + kind:
                fail(f"{where} source suffix must be .{kind}")
            shutil.copyfile(source, sd_out)
            if kind == "eaf" and sd_out.stat().st_size < 16:
                print(f"warning: {sd_out} is suspiciously small for an EAF", file=sys.stderr)

        entry["bytes"] = sd_out.stat().st_size
        manifest_assets.append(entry)
        print(f"{where}: {sd_rel} = {human_bytes(entry['bytes'])}")

    if args.dry_run:
        return 0

    if not args.no_manifest:
        manifest = {
            "schema_version": 1,
            "app_id": app_id,
            "assets": manifest_assets,
        }
        write_json(package_dir / "sd_manifest.json", manifest)
        print(f"wrote {package_dir / 'sd_manifest.json'}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
