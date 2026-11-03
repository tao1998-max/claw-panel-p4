#!/usr/bin/env python3
"""Generate 96x96 App icons (baseline JPEG 4:2:0) with pure-Python raster code.

No Pillow required: shapes are drawn into an RGB buffer at 4x, averaged down to
96x96 for anti-aliasing, written as PPM and encoded with ffmpeg.

    python tools/make_app_icons.py --out tools/app_icons/out
    python tools/make_app_icons.py --out /tmp/icons --ids answer_book blackjack

The icons are deliberately flat and geometric: at 96 px in a phone-style grid,
detail turns to mud, so each icon is one gradient background plus one glyph.
"""

from __future__ import annotations

import argparse
import math
import subprocess
import sys
from pathlib import Path

SS = 4                 # supersampling factor
SIZE = 96
W = SIZE * SS


class Canvas:
    def __init__(self, top: tuple[int, int, int], bottom: tuple[int, int, int],
                 glow: tuple[int, int, int] | None = None) -> None:
        self.px = bytearray(W * W * 3)
        for y in range(W):
            k = y / (W - 1)
            base = tuple(round(top[i] + (bottom[i] - top[i]) * k) for i in range(3))
            row = bytes(base) * W
            self.px[y * W * 3:(y + 1) * W * 3] = row
        if glow:
            cx, cy, radius = W * 0.28, W * 0.18, W * 0.75
            for y in range(W):
                for x in range(W):
                    d = math.hypot(x - cx, y - cy) / radius
                    if d >= 1:
                        continue
                    a = (1.0 - d) ** 2 * 0.45
                    i = (y * W + x) * 3
                    for c in range(3):
                        self.px[i + c] = min(255, round(self.px[i + c] * (1 - a) + glow[c] * a))

    def blend(self, x: int, y: int, color: tuple[int, int, int], alpha: float = 1.0) -> None:
        if x < 0 or y < 0 or x >= W or y >= W or alpha <= 0:
            return
        i = (y * W + x) * 3
        if alpha >= 1:
            self.px[i:i + 3] = bytes(color)
            return
        for c in range(3):
            self.px[i + c] = round(self.px[i + c] * (1 - alpha) + color[c] * alpha)

    def rounded_rect(self, x: float, y: float, w: float, h: float, r: float,
                     color: tuple[int, int, int], alpha: float = 1.0) -> None:
        x0, y0, x1, y1 = x, y, x + w, y + h
        for py in range(max(0, int(y0)), min(W, int(math.ceil(y1)))):
            for px in range(max(0, int(x0)), min(W, int(math.ceil(x1)))):
                cx = min(max(px + 0.5, x0 + r), x1 - r)
                cy = min(max(py + 0.5, y0 + r), y1 - r)
                if math.hypot(px + 0.5 - cx, py + 0.5 - cy) <= r:
                    self.blend(px, py, color, alpha)

    def circle(self, cx: float, cy: float, radius: float,
               color: tuple[int, int, int], alpha: float = 1.0) -> None:
        for py in range(max(0, int(cy - radius - 1)), min(W, int(cy + radius + 2))):
            for px in range(max(0, int(cx - radius - 1)), min(W, int(cx + radius + 2))):
                if math.hypot(px + 0.5 - cx, py + 0.5 - cy) <= radius:
                    self.blend(px, py, color, alpha)

    def ring(self, cx: float, cy: float, radius: float, width: float,
             color: tuple[int, int, int], alpha: float = 1.0,
             start: float = 0.0, end: float = math.tau) -> None:
        for py in range(max(0, int(cy - radius - width)), min(W, int(cy + radius + width + 2))):
            for px in range(max(0, int(cx - radius - width)), min(W, int(cx + radius + width + 2))):
                dx = px + 0.5 - cx
                dy = py + 0.5 - cy
                d = math.hypot(dx, dy)
                if abs(d - radius) > width / 2:
                    continue
                if start or end < math.tau:
                    ang = math.atan2(dy, dx) % math.tau
                    if not (start <= ang <= end):
                        continue
                self.blend(px, py, color, alpha)

    def line(self, x0: float, y0: float, x1: float, y1: float, width: float,
             color: tuple[int, int, int], alpha: float = 1.0) -> None:
        vx, vy = x1 - x0, y1 - y0
        length2 = vx * vx + vy * vy or 1.0
        minx, maxx = int(min(x0, x1) - width), int(max(x0, x1) + width)
        miny, maxy = int(min(y0, y1) - width), int(max(y0, y1) + width)
        for py in range(max(0, miny), min(W, maxy + 1)):
            for px in range(max(0, minx), min(W, maxx + 1)):
                t = ((px + 0.5 - x0) * vx + (py + 0.5 - y0) * vy) / length2
                t = max(0.0, min(1.0, t))
                d = math.hypot(px + 0.5 - (x0 + t * vx), py + 0.5 - (y0 + t * vy))
                if d <= width / 2:
                    self.blend(px, py, color, alpha)

    def polygon(self, points: list[tuple[float, float]],
                color: tuple[int, int, int], alpha: float = 1.0) -> None:
        ys = [p[1] for p in points]
        for py in range(max(0, int(min(ys))), min(W, int(max(ys)) + 1)):
            yc = py + 0.5
            xs = []
            for i, (x0, y0) in enumerate(points):
                x1, y1 = points[(i + 1) % len(points)]
                if (y0 <= yc < y1) or (y1 <= yc < y0):
                    xs.append(x0 + (yc - y0) * (x1 - x0) / (y1 - y0))
            xs.sort()
            for a, b in zip(xs[0::2], xs[1::2]):
                for px in range(max(0, int(a)), min(W, int(math.ceil(b)))):
                    self.blend(px, py, color, alpha)

    def save_jpeg(self, path: Path) -> None:
        small = bytearray(SIZE * SIZE * 3)
        for y in range(SIZE):
            for x in range(SIZE):
                r = g = b = 0
                for sy in range(SS):
                    base = ((y * SS + sy) * W + x * SS) * 3
                    for sx in range(SS):
                        i = base + sx * 3
                        r += self.px[i]
                        g += self.px[i + 1]
                        b += self.px[i + 2]
                n = SS * SS
                i = (y * SIZE + x) * 3
                small[i] = r // n
                small[i + 1] = g // n
                small[i + 2] = b // n

        ppm = path.with_suffix(".ppm")
        with open(ppm, "wb") as f:
            f.write(b"P6\n%d %d\n255\n" % (SIZE, SIZE))
            f.write(small)
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(ppm),
                        "-q:v", "3", "-pix_fmt", "yuvj420p", str(path)], check=True)
        ppm.unlink()


WHITE = (245, 249, 255)
CREAM = (255, 244, 224)
MUTED = (200, 214, 235)


def icon_answer_book() -> Canvas:
    c = Canvas((28, 46, 86), (12, 20, 40), (120, 150, 255))
    c.rounded_rect(W * .12, W * .30, W * .34, W * .42, W * .035, CREAM)
    c.rounded_rect(W * .54, W * .30, W * .34, W * .42, W * .035, CREAM)
    c.rounded_rect(W * .47, W * .29, W * .06, W * .44, W * .02, (150, 170, 210))
    for i in range(3):
        y = W * (0.40 + i * 0.08)
        c.line(W * .17, y, W * .40, y, W * .018, (120, 140, 180))
        c.line(W * .60, y, W * .83, y, W * .018, (120, 140, 180))
    return c


def icon_blackjack() -> Canvas:
    c = Canvas((96, 18, 32), (40, 8, 16), (255, 120, 120))
    c.rounded_rect(W * .27, W * .17, W * .46, W * .66, W * .05, WHITE)
    # heart: two circles + triangle
    c.circle(W * .44, W * .42, W * .085, (214, 40, 60))
    c.circle(W * .56, W * .42, W * .085, (214, 40, 60))
    c.polygon([(W * .355, W * .45), (W * .645, W * .45), (W * .50, W * .62)],
              (214, 40, 60))
    return c


def icon_countdown() -> Canvas:
    c = Canvas((10, 62, 70), (6, 26, 34), (90, 225, 210))
    c.ring(W * .50, W * .52, W * .30, W * .07, WHITE)
    c.line(W * .50, W * .52, W * .50, W * .34, W * .045, WHITE)
    c.line(W * .50, W * .52, W * .62, W * .58, W * .045, (255, 200, 110))
    c.circle(W * .50, W * .52, W * .035, WHITE)
    for i in range(4):
        ang = i * math.pi / 2
        x0 = W * .50 + math.cos(ang) * W * .24
        y0 = W * .52 + math.sin(ang) * W * .24
        x1 = W * .50 + math.cos(ang) * W * .27
        y1 = W * .52 + math.sin(ang) * W * .27
        c.line(x0, y0, x1, y1, W * .022, MUTED)
    return c


def icon_morse() -> Canvas:
    c = Canvas((22, 30, 84), (8, 12, 36), (140, 120, 255))
    c.circle(W * .30, W * .58, W * .075, (255, 210, 120))
    for i, r in enumerate((0.16, 0.27, 0.38)):
        c.ring(W * .30, W * .58, W * r, W * .035, (150, 200, 255),
               alpha=0.95 - i * 0.22, start=-0.62, end=0.62)
    c.line(W * .52, W * .34, W * .82, W * .34, W * .05, WHITE)
    c.line(W * .52, W * .50, W * .66, W * .50, W * .05, WHITE)
    c.line(W * .72, W * .50, W * .82, W * .50, W * .05, WHITE)
    c.line(W * .52, W * .66, W * .60, W * .66, W * .05, WHITE)
    c.line(W * .66, W * .66, W * .82, W * .66, W * .05, WHITE)
    return c


def icon_ashare() -> Canvas:
    c = Canvas((8, 42, 30), (4, 18, 14), (90, 230, 150))
    bars = [(0.18, 0.55, 0.28, 0.30, (70, 225, 140)),
            (0.36, 0.42, 0.24, 0.40, (255, 105, 110)),
            (0.54, 0.30, 0.20, 0.48, (70, 225, 140)),
            (0.72, 0.22, 0.24, 0.55, (70, 225, 140))]
    for x, top, wdt, height, color in bars:
        c.line(W * (x + wdt / 2), W * (top - 0.08), W * (x + wdt / 2),
               W * (top + height + 0.04), W * .02, color)
        c.rounded_rect(W * x, W * top, W * wdt, W * height, W * .02, color)
    c.polygon([(W * .14, W * .78), (W * .36, W * .68), (W * .56, W * .72),
               (W * .84, W * .40), (W * .84, W * .50), (W * .56, W * .82),
               (W * .36, W * .78), (W * .14, W * .88)], (255, 225, 130))
    return c


def icon_weather() -> Canvas:
    c = Canvas((36, 90, 160), (12, 30, 60), (255, 220, 140))
    c.circle(W * .62, W * .36, W * .16, (255, 205, 90))
    c.circle(W * .40, W * .58, W * .13, WHITE)
    c.circle(W * .56, W * .52, W * .17, WHITE)
    c.circle(W * .70, W * .60, W * .12, WHITE)
    c.rounded_rect(W * .30, W * .58, W * .52, W * .14, W * .07, WHITE)
    return c


def icon_mqtt() -> Canvas:
    c = Canvas((58, 26, 96), (20, 10, 40), (200, 140, 255))
    hub = (W * .50, W * .50)
    nodes = [(W * .22, W * .26), (W * .78, W * .26), (W * .22, W * .76), (W * .78, W * .76)]
    for x, y in nodes:
        c.line(hub[0], hub[1], x, y, W * .025, (170, 180, 255))
        c.circle(x, y, W * .07, (120, 220, 255))
    c.circle(hub[0], hub[1], W * .12, WHITE)
    c.circle(hub[0], hub[1], W * .06, (90, 110, 240))
    return c


def icon_radio() -> Canvas:
    c = Canvas((22, 10, 42), (4, 6, 16), (255, 43, 214))
    # antenna
    c.line(W * .50, W * .05, W * .66, W * .27, W * .025, (0, 229, 255))
    c.circle(W * .50, W * .05, W * .03, WHITE)
    # radio body: accent rim, dark face
    c.rounded_rect(W * .14, W * .28, W * .72, W * .56, W * .08, (0, 229, 255))
    c.rounded_rect(W * .17, W * .31, W * .66, W * .50, W * .06, (8, 12, 24))
    # speaker slots
    for i in range(4):
        c.rounded_rect(W * .22, W * .38 + i * W * .09, W * .28, W * .035,
                       W * .015, (0, 120, 150))
    # tuning dial
    c.circle(W * .68, W * .47, W * .13, (12, 20, 35))
    c.ring(W * .68, W * .47, W * .13, W * .02, (0, 229, 255))
    c.line(W * .68, W * .47, W * .74, W * .40, W * .018, (255, 43, 214))
    c.circle(W * .68, W * .47, W * .025, WHITE)
    # broadcast arcs
    c.ring(W * .68, W * .47, W * .20, W * .018, (255, 43, 214),
           alpha=0.8, start=-0.5, end=0.5)
    c.ring(W * .68, W * .47, W * .26, W * .014, (120, 80, 255),
           alpha=0.6, start=-0.5, end=0.5)
    # power LED
    c.circle(W * .24, W * .68, W * .022, (125, 255, 207))
    return c


def icon_sd_fallback() -> Canvas:
    c = Canvas((10, 16, 30), (4, 6, 14), (120, 90, 255))
    c.ring(W * .50, W * .50, W * .28, W * .045, (150, 210, 255), alpha=0.9)
    c.polygon([(W * .50, W * .30), (W * .66, W * .50), (W * .50, W * .70),
               (W * .34, W * .50)], (230, 240, 255))
    c.circle(W * .50, W * .50, W * .06, (120, 200, 255))
    return c


ICONS = {
    "answer_book": icon_answer_book,
    "blackjack": icon_blackjack,
    "countdown": icon_countdown,
    "morse_station": icon_morse,
    "ashare_watch": icon_ashare,
    "weather_panel": icon_weather,
    "mqtt_panel": icon_mqtt,
    "network_radio": icon_radio,
    "sd_fallback": icon_sd_fallback,
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, required=True, help="Output directory")
    parser.add_argument("--ids", nargs="*", choices=sorted(ICONS),
                        help="Subset of app ids (default: all)")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    ids = args.ids or sorted(ICONS)
    for app_id in ids:
        path = args.out / f"{app_id}.jpg"
        ICONS[app_id]().save_jpeg(path)
        print(f"{app_id}: {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
