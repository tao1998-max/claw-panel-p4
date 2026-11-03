#!/usr/bin/env python3
"""Regenerate the anim_lab example assets (stdlib only, ffmpeg for the icon).

  app/assets/anim/f0..f5.png            package fallback frames (6)
  app/assets/anim/loader.json           package Lottie (ring + one dot)
  sd/panel-assets/anim_lab/anim/f0..f11.png  SD upgrade frames (12)
  sd/panel-assets/anim_lab/anim/loader.json  SD Lottie (ring + two dots)
  app/assets/icon.jpg                   96x96 baseline JPEG
"""

from __future__ import annotations

import json
import math
import shutil
import struct
import subprocess
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
EXAMPLE = HERE.parent
APP_ASSETS = EXAMPLE / "app" / "assets"
SD_ANIM = EXAMPLE / "sd" / "panel-assets" / "anim_lab" / "anim"

SS = 2                     # supersampling factor
SIZE = 160                 # output frame size
BG = (10, 15, 20)
LIME = (200, 255, 62)
CYAN = (98, 212, 255)
FRAMES = 12


def blend(bg, fg, alpha):
    return tuple(int(round(b + (f - b) * alpha)) for b, f in zip(bg, fg))


def render_frame(index: int) -> bytes:
    """Two staggered radar rings, one lime and one cyan, plus a centre dot."""
    s = SIZE * SS
    row = bytearray()
    pixels = bytearray()

    def phase(i):
        return (i % FRAMES) / FRAMES

    p1 = phase(index)
    p2 = phase(index + FRAMES // 2)
    r1 = 14 + 70 * p1
    r2 = 14 + 70 * p2
    a1 = (1.0 - p1) ** 1.5
    a2 = (1.0 - p2) ** 1.5
    width = 3.0 * SS

    for y in range(s):
        for x in range(s):
            dx = x - s / 2 + 0.5
            dy = y - s / 2 + 0.5
            d = math.hypot(dx, dy)
            color = BG
            if abs(d - r2) <= width / 2:
                color = blend(color, CYAN, a2)
            if abs(d - r1) <= width / 2:
                color = blend(color, LIME, a1)
            if d <= 4.0 * SS:
                color = blend(color, LIME, 1.0)
            row.extend(color)
        pixels.extend(row)
        row.clear()

    # box downsample SS x SS
    out = bytearray()
    for y in range(SIZE):
        for x in range(SIZE):
            r = g = b = 0
            for yy in range(SS):
                base = ((y * SS + yy) * s + x * SS) * 3
                for xx in range(SS):
                    off = base + xx * 3
                    r += pixels[off]
                    g += pixels[off + 1]
                    b += pixels[off + 2]
            n = SS * SS
            out.extend((r // n, g // n, b // n))
    return bytes(out)


def write_png(path: Path, width: int, height: int, rgb: bytes) -> None:
    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)
        raw.extend(rgb[y * stride:(y + 1) * stride])

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(bytes(raw), 9)) +
           chunk(b"IEND", b""))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


def write_ppm(path: Path, width: int, height: int, rgb: bytes) -> None:
    header = f"P6\n{width} {height}\n255\n".encode()
    path.write_bytes(header + rgb)


def icon_pixels(size: int = 96) -> bytes:
    out = bytearray()
    for y in range(size):
        for x in range(size):
            dx = x - size / 2 + 0.5
            dy = y - size / 2 + 0.5
            d = math.hypot(dx, dy)
            color = BG
            if abs(d - 30) <= 3:
                color = LIME
            if math.hypot(dx - 21, dy + 21) <= 6:
                color = CYAN
            out.extend(color)
    return bytes(out)


def orbit_layer(ind: int, offset_deg: int, color, radius: int) -> dict:
    return {
        "ddd": 0, "ind": ind, "ty": 4, "nm": f"orbit-{ind}", "sr": 1,
        "ks": {
            "o": {"a": 0, "k": 100},
            "r": {"a": 1, "k": [
                {"t": 0, "s": [offset_deg], "e": [offset_deg + 360],
                 "i": {"x": [0.667], "y": [1]}, "o": {"x": [0.333], "y": [0]}},
                {"t": 90, "s": [offset_deg + 360]},
            ]},
            "p": {"a": 0, "k": [80, 80, 0]},
            "a": {"a": 0, "k": [0, 0, 0]},
            "s": {"a": 0, "k": [100, 100, 100]},
        },
        "ao": 0,
        "shapes": [{
            "ty": "gr",
            "it": [
                {"ty": "el", "p": {"a": 0, "k": [42, 0]},
                 "s": {"a": 0, "k": [radius, radius]}, "d": 1},
                {"ty": "fl", "c": {"a": 0, "k": color},
                 "o": {"a": 0, "k": 100}, "r": 1},
                {"ty": "tr", "p": {"a": 0, "k": [0, 0]},
                 "a": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [100, 100]},
                 "r": {"a": 0, "k": 0}, "o": {"a": 0, "k": 100},
                 "sk": {"a": 0, "k": 0}, "sa": {"a": 0, "k": 0}},
            ],
            "nm": "dot",
        }],
        "ip": 0, "op": 90, "st": 0, "bm": 0,
    }


def ring_layer() -> dict:
    return {
        "ddd": 0, "ind": 9, "ty": 4, "nm": "ring", "sr": 1,
        "ks": {
            "o": {"a": 0, "k": 100},
            "r": {"a": 0, "k": 0},
            "p": {"a": 0, "k": [80, 80, 0]},
            "a": {"a": 0, "k": [0, 0, 0]},
            "s": {"a": 0, "k": [100, 100, 100]},
        },
        "ao": 0,
        "shapes": [{
            "ty": "gr",
            "it": [
                {"ty": "el", "p": {"a": 0, "k": [0, 0]},
                 "s": {"a": 0, "k": [118, 118]}, "d": 1},
                {"ty": "st", "c": {"a": 0, "k": [0.784, 1, 0.243, 1]},
                 "o": {"a": 0, "k": 22}, "w": {"a": 0, "k": 3},
                 "lc": 2, "lj": 2, "ml": 4},
                {"ty": "tr", "p": {"a": 0, "k": [0, 0]},
                 "a": {"a": 0, "k": [0, 0]}, "s": {"a": 0, "k": [100, 100]},
                 "r": {"a": 0, "k": 0}, "o": {"a": 0, "k": 100},
                 "sk": {"a": 0, "k": 0}, "sa": {"a": 0, "k": 0}},
            ],
            "nm": "ring",
        }],
        "ip": 0, "op": 90, "st": 0, "bm": 0,
    }


def lottie(dots: int) -> dict:
    layers = [ring_layer()]
    colors = [[0.784, 1, 0.243, 1], [0.384, 0.831, 1, 1]]
    for i in range(dots):
        layers.append(orbit_layer(i + 1, i * (360 // dots),
                                  colors[i % len(colors)], 20))
    return {
        "v": "5.7.4", "fr": 30, "ip": 0, "op": 90, "w": 160, "h": 160,
        "nm": "loader", "ddd": 0, "assets": [], "layers": layers,
    }


def main() -> int:
    (APP_ASSETS / "anim").mkdir(parents=True, exist_ok=True)
    SD_ANIM.mkdir(parents=True, exist_ok=True)

    frames = [render_frame(i) for i in range(FRAMES)]
    for i, rgb in enumerate(frames):
        write_png(SD_ANIM / f"f{i}.png", SIZE, SIZE, rgb)
    for slot, src in enumerate(range(0, FRAMES, FRAMES // 6)):
        write_png(APP_ASSETS / "anim" / f"f{slot}.png", SIZE, SIZE, frames[src])

    (APP_ASSETS / "anim" / "loader.json").write_text(
        json.dumps(lottie(1), separators=(",", ":")), encoding="utf-8")
    (SD_ANIM / "loader.json").write_text(
        json.dumps(lottie(2), separators=(",", ":")), encoding="utf-8")

    ppm = HERE / "icon.ppm"
    write_ppm(ppm, 96, 96, icon_pixels())
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg:
        subprocess.run([ffmpeg, "-y", "-loglevel", "error", "-i", str(ppm),
                        "-q:v", "4", str(APP_ASSETS / "icon.jpg")], check=True)
        ppm.unlink()
    else:
        print("warning: ffmpeg not found; icon.ppm left in place")

    print(f"frames: {FRAMES} SD, 6 package; lottie: package 1 dot, SD 2 dots")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
