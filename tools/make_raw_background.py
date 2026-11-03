#!/usr/bin/env python3
"""Decode an image with ffmpeg and pack it as raw RGB565 / GRAY8.

Why: the ESP32-P4 JPEG decoder upsamples 4:2:0 chroma simply, so very dark,
smooth gradients can show colour blocks. Decoding on the PC and storing a raw
RGB565 file with an ordered 8x8 Bayer dither avoids that, removes runtime JPEG
decode work, and lets ``image.load_raw()`` + ``img:set_frame()`` hand the
buffer to LVGL without any copy.

    python tools/make_raw_background.py --input bg.jpg --output bg.rgb565
    python tools/make_raw_background.py --input bg.jpg --output bg.rgb565 \
        --width 1024 --height 600 --no-dither

Requires ffmpeg on PATH. The output is uncompressed: W*H*2 bytes for RGB565.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

# 8x8 ordered Bayer matrix (values 0..63).
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


def decode_ppm(path: Path, width: int | None, height: int | None) -> tuple[int, int, bytes]:
    with tempfile.TemporaryDirectory(prefix="rawbg-") as tmp:
        ppm = Path(tmp) / "frame.ppm"
        cmd = ["ffmpeg", "-y", "-loglevel", "error", "-i", str(path), "-frames:v", "1"]
        if width or height:
            cmd += ["-vf", f"scale={width or -1}:{height or -1}:flags=lanczos"]
        cmd += ["-pix_fmt", "rgb24", str(ppm)]
        subprocess.run(cmd, check=True)
        data = ppm.read_bytes()

    if not data.startswith(b"P6"):
        raise SystemExit("ffmpeg did not produce a binary PPM")
    fields: list[bytes] = []
    pos = 0
    while len(fields) < 3:
        end = data.index(b"\n", pos)
        line = data[pos:end].strip()
        pos = end + 1
        if line and not line.startswith(b"#"):
            fields.append(line)
    width_px, height_px = map(int, fields[1].split())
    if int(fields[2]) != 255:
        raise SystemExit("unexpected PPM depth")
    return width_px, height_px, data[pos:]


def quantize(value: int, bits: int, x: int, y: int, dither: bool) -> int:
    levels = (1 << bits) - 1
    if dither:
        threshold = (BAYER8[y & 7][x & 7] + 0.5) / 64.0 - 0.5
        q = int(value / 255.0 * levels + 0.5 + threshold)
    else:
        q = value * levels // 255
    return 0 if q < 0 else (levels if q > levels else q)


def write_rgb565(pixels: bytes, width: int, height: int, out: Path, dither: bool) -> None:
    data = bytearray(width * height * 2)
    for y in range(height):
        row = y * width
        for x in range(width):
            i = (row + x) * 3
            r, g, b = pixels[i], pixels[i + 1], pixels[i + 2]
            value = ((quantize(r, 5, x, y, dither) << 11) |
                     (quantize(g, 6, x, y, dither) << 5) |
                     quantize(b, 5, x, y, dither))
            o = (row + x) * 2
            data[o] = value & 0xFF
            data[o + 1] = (value >> 8) & 0xFF
    out.write_bytes(data)


def write_gray8(pixels: bytes, width: int, height: int, out: Path) -> None:
    data = bytearray(width * height)
    for i in range(width * height):
        r, g, b = pixels[i * 3], pixels[i * 3 + 1], pixels[i * 3 + 2]
        data[i] = (77 * r + 150 * g + 29 * b) >> 8
    out.write_bytes(data)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", type=Path, required=True, help="Source image (any ffmpeg format)")
    parser.add_argument("--output", type=Path, required=True,
                        help="Destination .rgb565 / .gray8 file")
    parser.add_argument("--width", type=int, help="Resize width (default: source width)")
    parser.add_argument("--height", type=int, help="Resize height (default: source height)")
    parser.add_argument("--format", choices=("rgb565", "gray8"), default="rgb565")
    parser.add_argument("--no-dither", action="store_true",
                        help="Disable the ordered Bayer dither (faster, visible banding)")
    args = parser.parse_args()

    width, height, pixels = decode_ppm(args.input, args.width, args.height)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.format == "rgb565":
        write_rgb565(pixels, width, height, args.output, not args.no_dither)
        expected = width * height * 2
    else:
        write_gray8(pixels, width, height, args.output)
        expected = width * height
    print(f"{args.output}: {width}x{height} {args.format} {expected} bytes "
          f"(dither={'off' if args.no_dither else 'on'})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
