#!/usr/bin/env python3
"""Generate the neon slot-machine assets (symbol JPEGs, app icon, OGG sfx).

Dependencies: Pillow, ffmpeg (on PATH).
Run from anywhere; outputs land in ../assets.
"""
import math
import os
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "assets"))
SYM_DIR = os.path.join(OUT, "symbols")
SFX_DIR = os.path.join(OUT, "sfx")

FONT_BOLD = "C:/Windows/Fonts/msyhbd.ttc"
FONT_REG = "C:/Windows/Fonts/msyh.ttc"

SIZE = 128            # symbol canvas
BG = (10, 14, 24, 255)
FRAME = (44, 58, 86, 255)


def rounded_rect_mask(w, h, r):
    m = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(m)
    d.rounded_rectangle([0, 0, w - 1, h - 1], radius=r, fill=255)
    return m


def glow_layer(shape_img, color, radius=10, boost=1.6):
    """Return a blurred, brightened copy of the shape for a neon halo."""
    rgb = Image.new("RGB", shape_img.size, (0, 0, 0))
    rgb.paste(color[:3], (0, 0), shape_img)
    rgb = rgb.filter(ImageFilter.GaussianBlur(radius))
    rgb = Image.eval(rgb, lambda v: min(255, int(v * boost)))
    return rgb


def draw_symbol(name, draw_fn, color, glow_radius=10):
    img = Image.new("RGBA", (SIZE, SIZE), BG)
    d = ImageDraw.Draw(img)

    # recessed rounded frame
    d.rounded_rectangle([3, 3, SIZE - 4, SIZE - 4], radius=18,
                        outline=FRAME, width=2)

    # symbol drawn on its own layer so the halo follows the shape
    sym = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    sd = ImageDraw.Draw(sym)
    draw_fn(sd)

    img = Image.alpha_composite(img, glow_layer(sym, color, glow_radius)
                                .convert("RGBA"))
    img = Image.alpha_composite(img, sym)
    img.convert("RGB").save(os.path.join(SYM_DIR, name + ".jpg"), quality=92)
    print("symbol", name)


def draw_seven(d):
    f = ImageFont.truetype(FONT_BOLD, 96)
    d.text((SIZE / 2, SIZE / 2 - 4), "7", font=f,
           fill=(255, 214, 102, 255), anchor="mm",
           stroke_width=6, stroke_fill=(255, 244, 200, 255))


def draw_bar(d):
    f = ImageFont.truetype(FONT_BOLD, 56)
    d.text((SIZE / 2, SIZE / 2), "BAR", font=f,
           fill=(255, 95, 109, 255), anchor="mm",
           stroke_width=5, stroke_fill=(255, 210, 216, 255))


def draw_coin(d):
    d.ellipse([18, 18, SIZE - 18, SIZE - 18], fill=(234, 179, 72, 255),
              outline=(255, 236, 170, 255), width=6)
    d.ellipse([30, 30, SIZE - 30, SIZE - 30], outline=(162, 112, 28, 255),
              width=4)
    f = ImageFont.truetype(FONT_BOLD, 60)
    d.text((SIZE / 2, SIZE / 2), "¥", font=f, fill=(255, 244, 190, 255),
           anchor="mm")


def draw_diamond(d):
    cx, cy = SIZE / 2, SIZE / 2
    pts = [(cx, 12), (SIZE - 14, cy), (cx, SIZE - 12), (14, cy)]
    d.polygon(pts, fill=(64, 190, 235, 255))
    d.polygon(pts, outline=(220, 250, 255, 255))
    d.line([(14, cy), (cx, 12)], fill=(160, 235, 255, 255), width=5)
    d.line([(14, cy), (cx, SIZE - 12)], fill=(110, 214, 250, 255), width=5)
    d.line([(SIZE - 14, cy), (cx, 12)], fill=(160, 235, 255, 255), width=5)
    d.line([(SIZE - 14, cy), (cx, SIZE - 12)], fill=(110, 214, 250, 255), width=5)


def draw_star(d):
    cx, cy = SIZE / 2, SIZE / 2
    pts = []
    for i in range(10):
        r = 54 if i % 2 == 0 else 22
        a = -math.pi / 2 + i * math.pi / 5
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    d.polygon(pts, fill=(176, 124, 255, 255),
              outline=(236, 224, 255, 255))


def draw_cherry(d):
    for (cx, cy) in ((38, 74), (90, 62)):
        d.ellipse([cx - 22, cy - 22, cx + 22, cy + 22],
                  fill=(255, 77, 109, 255),
                  outline=(255, 200, 212, 255), width=3)
        d.ellipse([cx - 9, cy - 12, cx + 3, cy - 4], fill=(255, 210, 220, 255))
    d.line([(64, 48), (64, 14)], fill=(80, 214, 130, 255), width=6)
    d.line([(64, 14), (44, 8)], fill=(80, 214, 130, 255), width=6)
    d.line([(64, 14), (84, 8)], fill=(80, 214, 130, 255), width=6)


def make_icon():
    img = Image.new("RGBA", (96, 96), (7, 10, 20, 255))
    d = ImageDraw.Draw(img)
    f = ImageFont.truetype(FONT_BOLD, 52)
    for i, (dx, color) in enumerate(((-16, (255, 95, 109)),
                                     (0, (234, 179, 72)),
                                     (16, (64, 190, 235)))):
        layer = Image.new("RGBA", (96, 96), (0, 0, 0, 0))
        ImageDraw.Draw(layer).text((48 + dx, 52), "7", font=f,
                                   fill=color + (255,), anchor="mm",
                                   stroke_width=4,
                                   stroke_fill=color + (120,))
        img = Image.alpha_composite(img, glow_layer(layer, color + (255,), 4, 1.8)
                                    .convert("RGBA"))
        img = Image.alpha_composite(img, layer)
    img.convert("RGB").save(os.path.join(OUT, "icon.jpg"), quality=90)
    print("icon")


# ------------------------- sfx -----------------------------------------

def ffmpeg(args):
    subprocess.run(["ffmpeg", "-y", "-v", "error", *args], check=True)


def gen_sfx():
    os.makedirs(SFX_DIR, exist_ok=True)
    sr = 16000
    # The firmware's sound path only demuxes OGG-Opus (the XiaoZhi AudioService
    # OggDemuxer feeds its Opus decode queue); OGG-Vorbis files are dropped
    # silently. Keep this libopus recipe in sync with the other example apps.
    opus = ["-c:a", "libopus", "-b:a", "24k", "-ar", str(sr), "-ac", "1"]

    # reel click: short bright tick
    ffmpeg(["-f", "lavfi", "-i",
            "aevalsrc=0.7*sin(1500*PI*t)*exp(-60*t):s=%d:d=0.06" % sr,
            *opus,
            os.path.join(SFX_DIR, "tick.ogg")])

    # reel stop: low thunk
    ffmpeg(["-f", "lavfi", "-i",
            "aevalsrc=0.9*sin(150*2*PI*t)*exp(-14*t):s=%d:d=0.22" % sr,
            *opus,
            os.path.join(SFX_DIR, "stop.ogg")])

    # spin whoosh: rising chirp with decay
    ffmpeg(["-f", "lavfi", "-i",
            "aevalsrc=0.35*sin(2*PI*(180+1200*t)*t)*exp(-2.5*t):s=%d:d=0.5" % sr,
            *opus,
            os.path.join(SFX_DIR, "spin.ogg")])

    # win: C-E-G arpeggio
    notes = [(660, 0), (880, 130), (1320, 260)]
    inputs, filters = [], []
    for i, (freq, dly) in enumerate(notes):
        inputs += ["-f", "lavfi", "-i",
                   "sine=frequency=%d:duration=0.14:sample_rate=%d" % (freq, sr)]
        seg = "[%d:a]" % i
        if dly:
            seg += "adelay=%d:all=1," % dly
        filters.append(seg + "afade=out:st=0.05:d=0.09[c%d]" % i)
    fc = ";".join(filters) + ";" + \
         "".join("[c%d]" % i for i in range(len(notes))) + \
         "amix=inputs=%d:normalize=0,afade=out:st=0.42:d=0.2" % len(notes)
    ffmpeg(inputs + ["-filter_complex", fc, *opus,
                     os.path.join(SFX_DIR, "win.ogg")])

    # jackpot: five-note fanfare
    notes = [(523, 0), (659, 110), (784, 220), (1046, 330), (1318, 440)]
    inputs, filters = [], []
    for i, (freq, dly) in enumerate(notes):
        inputs += ["-f", "lavfi", "-i",
                   "sine=frequency=%d:duration=0.16:sample_rate=%d" % (freq, sr)]
        seg = "[%d:a]" % i
        if dly:
            seg += "adelay=%d:all=1," % dly
        filters.append(seg + "afade=out:st=0.06:d=0.1[c%d]" % i)
    fc = ";".join(filters) + ";" + \
         "".join("[c%d]" % i for i in range(len(notes))) + \
         "amix=inputs=%d:normalize=0,afade=out:st=0.66:d=0.35" % len(notes)
    ffmpeg(inputs + ["-filter_complex", fc, *opus,
                     os.path.join(SFX_DIR, "jackpot.ogg")])
    print("sfx")


def main():
    os.makedirs(SYM_DIR, exist_ok=True)
    os.makedirs(OUT, exist_ok=True)
    draw_symbol("seven", draw_seven, (255, 200, 80), 10)
    draw_symbol("bar", draw_bar, (255, 80, 100), 10)
    draw_symbol("coin", draw_coin, (240, 190, 80), 12)
    draw_symbol("diamond", draw_diamond, (70, 200, 245), 12)
    draw_symbol("star", draw_star, (180, 120, 255), 12)
    draw_symbol("cherry", draw_cherry, (255, 70, 110), 12)
    make_icon()
    gen_sfx()


if __name__ == "__main__":
    sys.exit(main())
