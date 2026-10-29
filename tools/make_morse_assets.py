#!/usr/bin/env python3
"""Generate the Morse Station background and telegraph sound effects.

Outputs:
  apps/morse_station/assets/img/bg.jpg       1024x600 baseline JPEG
  apps/morse_station/assets/sfx/*.ogg        16 kHz mono OGG/Opus

Requires Pillow, ffmpeg (libopus) and numpy only for the optional grain pass.
The background is deliberately quiet: the live tree and the brass key are the
hero, the art is the radio-room paper texture behind them.
"""

from __future__ import annotations

import math
import os
import random
import shutil
import subprocess
import sys
import wave
from pathlib import Path

try:
    from PIL import Image, ImageChops, ImageDraw, ImageFilter
except ImportError:
    sys.exit("Pillow is required: pip install pillow")

ROOT = Path(__file__).resolve().parent.parent
APP = ROOT / "apps" / "morse_station"
IMG = APP / "assets" / "img" / "bg.jpg"
SFX = APP / "assets" / "sfx"
TMP = ROOT / "build" / "morse_sfx_wav"
SR = 16000
random.seed(7)


# --------------------------------------------------------------- background

def add_glow(img: Image.Image, cx: int, cy: int, radius: int,
             color: tuple[int, int, int], strength: float) -> Image.Image:
    mask = Image.new("L", (img.width, img.height), 0)
    d = ImageDraw.Draw(mask)
    d.ellipse([cx - radius, cy - radius, cx + radius, cy + radius],
              fill=int(255 * strength))
    mask = mask.filter(ImageFilter.GaussianBlur(radius * 0.55))
    glow = Image.new("RGBA", (img.width, img.height), (*color, 0))
    glow.putalpha(mask)
    return Image.alpha_composite(img.convert("RGBA"), glow).convert("RGB")


def make_background() -> Image.Image:
    W, H = 1024, 600
    top = (9, 22, 15)
    bottom = (2, 6, 4)
    strip = Image.new("RGB", (1, H))
    for y in range(H):
        t = y / (H - 1)
        strip.putpixel((0, y), tuple(
            int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)))
    img = strip.resize((W, H), Image.NEAREST)

    img = add_glow(img, 360, 300, 430, (16, 48, 32), 0.60)
    img = add_glow(img, 850, 250, 310, (18, 40, 28), 0.42)
    img = add_glow(img, 120, 90, 260, (15, 34, 24), 0.32)

    # fine etched grid
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for x in range(0, W, 32):
        alpha = 38 if x % 128 == 0 else 20
        d.line([(x, 0), (x, H)], fill=(22, 52, 40, alpha), width=1)
    for y in range(0, H, 32):
        alpha = 38 if y % 128 == 0 else 20
        d.line([(0, y), (W, y)], fill=(22, 52, 40, alpha), width=1)
    img = Image.alpha_composite(img.convert("RGBA"), layer).convert("RGB")

    # circuit traces, kept to margins / header so the live UI stays dominant
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    traces = [
        [(16, 96), (16, 492), (40, 492)],
        [(8, 150), (8, 330), (24, 330)],
        [(612, 70), (700, 70), (700, 52), (748, 52)],
        [(812, 70), (858, 70), (858, 40), (904, 40)],
        [(936, 70), (978, 70), (978, 54), (1006, 54)],
        [(1008, 130), (1008, 470)],
        [(1004, 470), (1004, 560), (984, 560)],
        [(10, 560), (10, 590), (30, 590)],
    ]
    for pts in traces:
        d.line(pts, fill=(26, 64, 48, 100), width=1, joint="curve")
        for px, py in (pts[0], pts[-1]):
            d.ellipse([px - 2, py - 2, px + 2, py + 2],
                      fill=(30, 72, 54, 110))
    # a small signal trace in the empty strip under the header
    start = 640
    pts = []
    for i in range(0, 340, 3):
        x = start + i
        y = 96 + math.sin(i / 13.0) * 3 * math.sin(i / 47.0)
        pts.append((x, y))
    d.line(pts, fill=(40, 98, 72, 55), width=1)
    img = Image.alpha_composite(img.convert("RGBA"), layer).convert("RGB")

    # vignette: keep the centre, settle the outer 8-10 %
    vign = Image.new("L", (W, H), 0)
    vd = ImageDraw.Draw(vign)
    vd.ellipse([-220, -150, W + 220, H + 150], fill=255)
    vign = vign.filter(ImageFilter.GaussianBlur(150))
    vign = vign.point(lambda v: 162 + int(v * 93 / 255))
    dark = Image.new("RGB", (W, H), (0, 0, 0))
    img = Image.composite(img, dark, vign)

    # technical frame + registration crosses
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    d.rectangle([10, 10, W - 11, H - 11], outline=(68, 54, 30, 130), width=1)
    d.rectangle([14, 14, W - 15, H - 15], outline=(24, 50, 39, 115), width=1)
    for cx, cy in ((22, 22), (W - 23, 22), (22, H - 23), (W - 23, H - 23)):
        d.line([(cx - 6, cy), (cx + 6, cy)], fill=(120, 96, 48, 120), width=1)
        d.line([(cx, cy - 6), (cx, cy + 6)], fill=(120, 96, 48, 120), width=1)
    # bottom ruler
    for x in range(24, W - 24, 16):
        h = 4 if x % 64 == 0 else 2
        alpha = 58 if x % 64 == 0 else 32
        d.line([(x, H - 9), (x, H - 9 - h)], fill=(64, 124, 94, alpha), width=1)
    img = Image.alpha_composite(img.convert("RGBA"), layer).convert("RGB")

    # very light grain so the dark gradient does not band
    noise = Image.effect_noise((W, H), 7).convert("RGB")
    img = Image.blend(img, noise, 0.03)

    return img


def save_background(img: Image.Image) -> None:
    IMG.parent.mkdir(parents=True, exist_ok=True)
    for quality in (82, 76, 70, 64):
        img.save(IMG, "JPEG", quality=quality, subsampling=2,
                 optimize=True, progressive=False)
        size = IMG.stat().st_size
        if size <= 120 * 1024:
            print(f"{IMG} q={quality} {size} bytes")
            return
    print(f"warning: background is {IMG.stat().st_size} bytes")


# --------------------------------------------------------------- sound

def env(n: int, attack: float = 0.006, release: float = 0.04) -> list[float]:
    a = max(1, int(SR * attack))
    r = max(1, int(SR * release))
    out = [1.0] * n
    for i in range(min(a, n)):
        out[i] = i / a
    for i in range(min(r, n)):
        out[n - 1 - i] = min(out[n - 1 - i], i / r)
    return out


def add_tone(buf: list[float], start: int, dur: float, freq: float, amp: float,
             harmonics=((1, 1.0), (2, 0.14), (3, 0.04)),
             attack: float = 0.006, release: float = 0.04) -> None:
    n = int(dur * SR)
    e = env(n, attack, release)
    for i in range(n):
        t = i / SR
        phase = 2.0 * math.pi * freq * t
        s = 0.0
        for h, ha in harmonics:
            s += ha * math.sin(phase * h)
        idx = start + i
        if idx < len(buf):
            buf[idx] += amp * s * e[i]


def normalize(buf: list[float], peak: float = 0.9) -> list[float]:
    m = max(1e-9, max(abs(x) for x in buf))
    if m > peak:
        scale = peak / m
        for i in range(len(buf)):
            buf[i] *= scale
    return buf


def write_wav(name: str, buf: list[float]) -> str:
    TMP.mkdir(parents=True, exist_ok=True)
    path = TMP / f"{name}.wav"
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(b"".join(
            int(max(-32768, min(32767, x * 32767))).to_bytes(2, "little", signed=True)
            for x in buf))
    return str(path)


def encode(name: str, buf: list[float]) -> None:
    wav = write_wav(name, normalize(buf))
    ogg = SFX / f"{name}.ogg"
    subprocess.run([
        "ffmpeg", "-y", "-loglevel", "error", "-i", wav,
        "-c:a", "libopus", "-b:a", "24k", "-ar", str(SR), "-ac", "1",
        "-application", "audio", "-frame_duration", "60", str(ogg),
    ], check=True)
    print(f"{name}.ogg {ogg.stat().st_size} bytes")


def tone(dur: float, freq: float, amp: float, **kw) -> list[float]:
    buf = [0.0] * int(dur * SR)
    add_tone(buf, 0, dur, freq, amp, **kw)
    return buf


def make_dot() -> list[float]:
    buf = tone(0.13, 760.0, 0.32, attack=0.004, release=0.045)
    for i in range(int(0.008 * SR)):
        t = i / SR
        buf[i] += 0.12 * random.uniform(-1, 1) * math.exp(-t * 180)
    return buf


def make_dash() -> list[float]:
    buf = tone(0.40, 760.0, 0.30, attack=0.004, release=0.05)
    for i in range(int(0.008 * SR)):
        t = i / SR
        buf[i] += 0.10 * random.uniform(-1, 1) * math.exp(-t * 180)
    return buf


def make_key() -> list[float]:
    n = int(0.04 * SR)
    buf = [0.0] * n
    lp = 0.0
    for i in range(n):
        t = i / SR
        x = random.uniform(-1.0, 1.0)
        lp += 0.42 * (x - lp)
        buf[i] = (x - lp) * 0.55 * math.exp(-t * 55)
        buf[i] += 0.22 * math.sin(2 * math.pi * 1580 * t) * math.exp(-t * 90)
    return buf


def make_ok() -> list[float]:
    buf = [0.0] * int(0.34 * SR)
    add_tone(buf, 0, 0.09, 880.0, 0.26, release=0.03)
    add_tone(buf, int(0.10 * SR), 0.20, 1320.0, 0.26, release=0.07)
    return buf


def make_err() -> list[float]:
    n = int(0.34 * SR)
    buf = [0.0] * n
    phase = 0.0
    for i in range(n):
        t = i / SR
        freq = 170.0 - 45.0 * (t / 0.34)
        phase += freq / SR
        saw = 2.0 * (phase % 1.0) - 1.0
        trem = 0.7 + 0.3 * math.sin(2 * math.pi * 28 * t)
        e = min(1.0, t / 0.006) * math.exp(-t * 5.5)
        buf[i] = (0.18 * saw + 0.05 * random.uniform(-1, 1)) * trem * e
    return buf


def make_start() -> list[float]:
    n = int(0.36 * SR)
    buf = [0.0] * n
    phase = 0.0
    for i in range(n):
        t = i / SR
        freq = 420.0 + (1120.0 - 420.0) * (t / 0.36)
        phase += 2.0 * math.pi * freq / SR
        e = min(1.0, t / 0.02) * math.exp(-t * 3.2)
        buf[i] = 0.24 * math.sin(phase) * e
    return buf


def make_over() -> list[float]:
    buf = [0.0] * int(0.72 * SR)
    add_tone(buf, 0, 0.14, 659.25, 0.22, release=0.04)
    add_tone(buf, int(0.14 * SR), 0.14, 440.0, 0.22, release=0.04)
    add_tone(buf, int(0.28 * SR), 0.40, 220.0, 0.22,
             harmonics=((1, 1.0), (2, 0.2)), release=0.14)
    return buf


def main() -> int:
    if shutil.which("ffmpeg") is None:
        sys.exit("ffmpeg not found")
    save_background(make_background())
    SFX.mkdir(parents=True, exist_ok=True)
    encode("dot", make_dot())
    encode("dash", make_dash())
    encode("key", make_key())
    encode("ok", make_ok())
    encode("err", make_err())
    encode("start", make_start())
    encode("over", make_over())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
