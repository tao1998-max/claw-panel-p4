#!/usr/bin/env python3
"""Render static design previews of the Morse Station App.

These are host-side mockups drawn with the same coordinates and bundled fonts
as apps/morse_station/scripts/main.lua.  They are *not* device screenshots;
the panel has no screenshot API.  Use them to review composition and colour,
then confirm the real thing on the panel.

Outputs docs/morse_station/preview_{game,menu,over}.png.
"""

from __future__ import annotations

import math
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("Pillow is required: pip install pillow")

ROOT = Path(__file__).resolve().parent.parent
APP = ROOT / "apps" / "morse_station"
OUT = ROOT / "docs" / "morse_station"
W, H = 1024, 600

BG_PATH = APP / "assets" / "img" / "bg.jpg"
UI_FONT = APP / "assets" / "ui_cn.ttf"
DISPLAY_FONT = APP / "assets" / "display.ttf"

C = {
    "panel": (7, 17, 13),
    "panel2": (10, 24, 18),
    "card": (10, 24, 18),
    "line": (23, 51, 38),
    "line_dim": (16, 37, 27),
    "node": (9, 22, 15),
    "node_border": (36, 80, 60),
    "letter": (169, 201, 185),
    "mint": (111, 240, 184),
    "mint_dim": (63, 157, 118),
    "mint_deep": (29, 92, 66),
    "brass": (217, 181, 109),
    "brass_hi": (239, 207, 138),
    "brass_dim": (122, 95, 48),
    "cream": (242, 236, 218),
    "muted": (124, 148, 136),
    "dim": (79, 106, 94),
    "danger": (255, 107, 92),
    "amber": (255, 180, 84),
    "tape": (5, 11, 8),
    "bg": (4, 8, 6),
}

MORSE = {
    "A": ".-", "B": "-...", "C": "-.-.", "D": "-..", "E": ".", "F": "..-.",
    "G": "--.", "H": "....", "I": "..", "J": ".---", "K": "-.-", "L": ".-..",
    "M": "--", "N": "-.", "O": "---", "P": ".--.", "Q": "--.-", "R": ".-.",
    "S": "...", "T": "-", "U": "..-", "V": "...-", "W": ".--", "X": "-..-",
    "Y": "-.--", "Z": "--..",
}
LETTERS = list(MORSE)
LETTER_BY_CODE = {v: k for k, v in MORSE.items()}

FONTS: dict[int, ImageFont.FreeTypeFont] = {}


def font(px: int, display: bool = False):
    key = px + (1000 if display else 0)
    if key not in FONTS:
        FONTS[key] = ImageFont.truetype(
            str(DISPLAY_FONT if display else UI_FONT), px)
    return FONTS[key]


def lh(px: int, display: bool = False) -> int:
    f = font(px, display)
    a, d = f.getmetrics()
    return a + d


def color(name_or_tuple):
    if isinstance(name_or_tuple, tuple):
        return name_or_tuple
    return C[name_or_tuple]


def panel(img: Image.Image, box, fill="panel", fill_alpha=238,
          outline="line", width=1, radius=18) -> Image.Image:
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    f = color(fill)
    o = color(outline)
    d.rounded_rectangle(box, radius=radius, fill=(*f, fill_alpha),
                        outline=(*o, 255), width=width)
    return Image.alpha_composite(img.convert("RGBA"), layer)


def text(draw, xy, s, px, col, anchor="lm", display=False):
    draw.text(xy, s, font=font(px, display), fill=color(col), anchor=anchor)


def draw_game(img: Image.Image, state="correct") -> Image.Image:
    d = ImageDraw.Draw(img)

    # header ------------------------------------------------------
    text(d, (36, 10 + lh(36, True) // 2), "MORSE STATION", 36,
         "cream", "lm", True)
    text(d, (38, 54 + lh(16) // 2), "电 码 站　·　7.023 MHz", 16,
         "mint_dim")
    d.ellipse([390, 22, 398, 30], fill=C["mint"])
    text(d, (404, 10 + lh(20) // 2), "发报训练", 20, "cream")
    text(d, (404, 42 + lh(16) // 2), "第 03 号电文　·　LV 1", 16, "muted")

    text(d, (680, 6 + lh(16) // 2), "得 分", 16, "muted")
    text(d, (680, 22 + lh(40) // 2), "01240", 40, "cream")
    text(d, (806, 6 + lh(16) // 2), "连 击", 16, "muted")
    text(d, (806, 30 + lh(20) // 2), "×1.2", 20, "mint")
    text(d, (910, 6 + lh(16) // 2), "信 号", 16, "muted")
    for i in range(3):
        x = 910 + i * 24
        d.ellipse([x, 38, x + 18, 56], fill=C["mint"],
                  outline=C["mint_dim"], width=1)
    d.line([(36, 84), (988, 84)], fill=C["line_dim"], width=1)
    d.line([(36, 84), (148, 84)], fill=C["brass_dim"], width=2)

    # tree panel -------------------------------------------------
    img = panel(img, [28, 104, 695, 499], "panel", 238, "line", 1, 18)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([40, 116, 683, 487], radius=14,
                        outline=(*C["line_dim"], 255), width=1)
    text(d, (48, 116 + lh(16) // 2), "解 码 树", 16, "mint_dim")
    text(d, (664, 116 + lh(16) // 2), "26 字 母 · 4 层", 16, "dim", "rm")
    text(d, (48, 460 + lh(16) // 2),
         "左 · 点　　右 − 划　　路径点亮 = 当前电码", 16, "dim")

    # tree geometry, identical to the Lua constants
    cx, y0, dy = 362, 172, 64
    by_depth = {}
    level = [""]
    by_depth[0] = [""]
    for depth in range(1, 5):
        nxt = []
        for code in level:
            nxt += [code + ".", code + "-"]
        by_depth[depth] = nxt
        level = nxt
    pos = {}
    for depth in range(0, 5):
        lst = by_depth[depth]
        n = len(lst)
        spacing = 0 if depth == 0 else 608 / n
        for i, code in enumerate(lst, start=1):
            if depth == 0:
                pos[code] = (cx, y0)
            else:
                pos[code] = (cx + (i - (n + 1) / 2) * spacing, y0 + depth * dy)

    path_code = "-.-" if state == "correct" else "-.-"
    path_codes = {path_code[:i] for i in range(0, len(path_code) + 1)}
    target = "K"

    # edges: dim first, then lit path
    for depth in range(1, 5):
        for code in by_depth[depth]:
            a = pos[code[:-1]]
            b = pos[code]
            lit = code in path_codes
            d.line([a, b], fill=C["mint"] if lit else C["line"],
                   width=4 if lit else 2)

    radius = {0: 9, 1: 18, 2: 16, 3: 14, 4: 12}
    for depth in range(0, 5):
        for code in by_depth[depth]:
            letter = LETTER_BY_CODE.get(code)
            empty = depth == 4 and not letter
            x, y = pos[code]
            if empty:
                d.ellipse([x - 5, y - 5, x + 5, y + 5],
                          outline=C["line"], width=1)
                continue
            r = radius[depth]
            lit = code in path_codes
            is_target = code == path_code
            fill = (36, 31, 14) if is_target else ((14, 42, 30) if lit else C["node"])
            border = C["brass"] if is_target else (C["mint"] if lit else C["node_border"])
            d.ellipse([x - r, y - r, x + r, y + r], fill=fill,
                      outline=border, width=2 if lit or is_target else 1)
            if depth == 0:
                d.ellipse([x - 3, y - 3, x + 3, y + 3], fill=C["brass"])
            else:
                col = C["brass"] if is_target else (C["cream"] if lit else C["letter"])
                px = 20 if depth <= 2 else 16
                text(d, (x, y), letter, px, col, "mm")

    # console ----------------------------------------------------
    img = panel(img, [708, 104, 995, 499], "panel", 238, "line", 1, 18)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([720, 116, 983, 487], radius=14,
                        outline=(*C["line_dim"], 255), width=1)
    text(d, (728, 116 + lh(16) // 2), "报 文", 16, "mint_dim")
    text(d, (976, 116 + lh(16) // 2), "7.023 MHz", 16, "dim", "rm")

    card = [726, 146, 977, 291]
    d.rounded_rectangle(card, radius=14, fill=C["card"],
                        outline=C["line"], width=1)
    for (x, y, hx, hy) in ((730, 150, 1, 1), (973, 150, -1, 1),
                           (730, 287, 1, -1), (973, 287, -1, -1)):
        d.line([(x, y), (x + 14 * hx, y)], fill=C["brass_dim"], width=2)
        d.line([(x, y), (x, y + 14 * hy)], fill=C["brass_dim"], width=2)
    text(d, (740, 154 + lh(16) // 2), "目 标", 16, "muted")
    text(d, (963, 154 + lh(16) // 2), "SEND", 16, "dim", "rm")
    text(d, (852, 219 + 6), "K", 96, "cream", "mm", True)

    # success ring around the card
    d.rounded_rectangle([722, 142, 981, 295], radius=18,
                        outline=(*C["mint"], 120), width=2)

    text(d, (726, 296 + lh(16) // 2), "纸 带", 16, "muted")
    d.rounded_rectangle([726, 322, 977, 365], radius=8,
                        fill=C["tape"], outline=C["line"], width=1)
    slots = [748, 802, 856, 910]
    for i, sx in enumerate(slots):
        d.rounded_rectangle([sx, 331, sx + 46, 356], radius=6,
                            fill=(5, 11, 8), outline=C["line_dim"], width=1)
        if i < len(path_code):
            sym = path_code[i]
            if sym == ".":
                d.ellipse([sx + 15, 337, sx + 31, 353],
                          fill=C["tape"], outline=C["mint"], width=2)
            else:
                d.rounded_rectangle([sx + 7, 339, sx + 39, 349], radius=5,
                                    fill=C["tape"], outline=C["mint"], width=2)

    text(d, (852, 374 + lh(20) // 2), "发报正确  +178", 20, "mint", "mm")
    text(d, (852, 404 + lh(16) // 2), "K = − · −", 16, "cream", "mm")
    d.rounded_rectangle([726, 432, 977, 439], radius=4, fill=(12, 26, 20))
    d.rounded_rectangle([726, 432, 877, 439], radius=4, fill=C["mint"])
    for tx in (789, 852, 915):
        d.line([(tx, 432), (tx, 439)], fill=C["panel"], width=1)
    text(d, (726, 448 + lh(16) // 2), "信 号 窗 口", 16, "dim")
    text(d, (977, 448 + lh(16) // 2), "7.6s", 16, "muted", "rm")

    # right controls --------------------------------------------
    text(d, (720, 504 + lh(16) // 2), "最佳 01240", 16, "muted")
    for bx, label in ((720, "提示 -30%"), (864, "菜单")):
        d.rounded_rectangle([bx, 526, bx + 132, 569], radius=12,
                            fill=C["panel2"], outline=C["line"], width=1)
        text(d, (bx + 66, 548), label, 16,
             "brass" if bx == 720 else "cream", "mm")
    text(d, (720, 572 + lh(16) // 2), "命中 12/15", 16, "dim")

    # telegraph key ---------------------------------------------
    d.rounded_rectangle([28, 508, 707, 591], radius=16,
                        fill=(10, 21, 16), outline=(58, 90, 68), width=1)
    text(d, (56, 520 + lh(20) // 2), "发 报 键", 20, "brass")
    text(d, (56, 550 + lh(16) // 2), "轻点 ·　按住 −", 16, "muted")
    d.ellipse([218, 554, 238, 574], fill=(10, 21, 16),
              outline=C["brass_dim"], width=1)
    d.rounded_rectangle([228, 562, 628, 565], radius=2, fill=C["brass_dim"])
    d.ellipse([620, 540, 668, 588], fill=(27, 22, 9),
              outline=C["brass"], width=2)
    d.ellipse([627, 547, 661, 581], fill=(42, 34, 16),
              outline=C["brass_dim"], width=1)
    text(d, (644, 564), "·−", 16, "brass_dim", "mm")
    d.rounded_rectangle([228, 580, 607, 582], radius=1, fill=(13, 29, 21))
    d.rounded_rectangle([228, 580, 400, 582], radius=1, fill=C["mint"])
    d.line([(418, 580), (418, 582)], fill=C["brass_dim"], width=1)

    # score popup
    text(d, (920, 210), "+178", 20, "mint", "lm")

    return img


def draw_menu(img: Image.Image) -> Image.Image:
    layer = Image.new("RGBA", img.size, (2, 6, 4, 222))
    img = Image.alpha_composite(img.convert("RGBA"), layer)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([172, 64, 851, 535], radius=26,
                        fill=C["panel"], outline=C["line"], width=1)
    d.rounded_rectangle([184, 76, 839, 523], radius=20,
                        outline=C["line_dim"], width=1)
    text(d, (512, 64 + 24 + lh(16) // 2), "SIGNAL STATION · 7.023 MHz",
         16, "brass_dim", "mm")
    text(d, (512, 64 + 44 + lh(96, True) // 2), "电 码 站", 96,
         "cream", "mm", True)
    text(d, (512, 64 + 186 + lh(20) // 2), "把一束电波，敲成一句话。",
         20, "muted", "mm")

    cards = [
        (212, "发 报", "看字 · 敲出电码", "训练手键记忆", "A", "· −", "mint"),
        (520, "抄 收", "看灯 · 还原电文", "训练听辨节奏", "?", "− · −", "amber"),
    ]
    for x, title, sub, hint, big, code, col in cards:
        d.rounded_rectangle([x, 292, x + 291, 439], radius=16,
                            fill=(11, 25, 19), outline=C["line"], width=1)
        text(d, (x + 22, 292 + 12 + lh(40) // 2), title, 40, "cream")
        text(d, (x + 22, 292 + 76 + lh(16) // 2), sub, 16, "muted")
        text(d, (x + 22, 292 + 104 + lh(16) // 2), hint, 16, "dim")
        text(d, (x + 218, 292 + 12 + lh(40) // 2), big, 40, col)
        text(d, (x + 220, 292 + 76 + lh(20) // 2), code, 20, col)

    text(d, (512, 64 + 384 + lh(20) // 2),
         "电键：轻点 = ·　　按住 = −", 20, "cream", "mm")
    text(d, (512, 64 + 414 + lh(16) // 2),
         "本台最高 01240", 16, "muted", "mm")
    text(d, (512, 64 + 438 + lh(16) // 2),
         "26 个字母 · 3 盏信号灯 · 连击加成", 16, "dim", "mm")

    # sonar rings in the top-right corner
    d.ellipse([786, 78, 825, 117], outline=C["mint_deep"], width=1)
    d.ellipse([762, 70, 817, 125], outline=C["line"], width=1)
    return img.convert("RGB")


def draw_over(img: Image.Image) -> Image.Image:
    layer = Image.new("RGBA", img.size, (2, 6, 4, 222))
    img = Image.alpha_composite(img.convert("RGBA"), layer)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([242, 104, 781, 495], radius=24,
                        fill=C["panel"], outline=C["line"], width=1)
    d.rounded_rectangle([254, 116, 769, 483], radius=18,
                        outline=C["line_dim"], width=1)
    text(d, (512, 104 + 24 + lh(16) // 2), "TRANSMISSION ENDED",
         16, "brass_dim", "mm")
    text(d, (512, 104 + 48 + lh(40) // 2), "电 文 中 断",
         40, "cream", "mm")
    text(d, (512, 104 + 106 + lh(16) // 2), "本 局 得 分",
         16, "muted", "mm")
    text(d, (512, 104 + 128 + lh(96, True) // 2), "01320",
         96, "mint", "mm", True)
    text(d, (512, 104 + 272 + lh(20) // 2),
         "命中 12/15　最高连击 7　准确率 80%", 20, "muted", "mm")
    text(d, (512, 104 + 300 + lh(20) // 2), "新 纪 录", 20, "brass", "mm")

    d.rounded_rectangle([302, 430, 501, 479], radius=14,
                        fill=C["brass"], outline=C["brass_dim"], width=1)
    text(d, (401, 455), "再来一局", 20, (10, 21, 16), "mm")
    d.rounded_rectangle([522, 430, 721, 479], radius=14,
                        fill=C["panel2"], outline=C["line"], width=1)
    text(d, (621, 455), "返回菜单", 20, "cream", "mm")
    return img.convert("RGB")


def main() -> int:
    bg = Image.open(BG_PATH).convert("RGB")
    OUT.mkdir(parents=True, exist_ok=True)

    game = draw_game(bg.copy(), "correct")
    game.save(OUT / "preview_game.png")

    menu = draw_menu(game.convert("RGBA"))
    menu.save(OUT / "preview_menu.png")

    over_scene = draw_game(bg.copy(), "wrong")
    over = draw_over(over_scene.convert("RGBA"))
    over.save(OUT / "preview_over.png")

    print("wrote", OUT / "preview_game.png")
    print("wrote", OUT / "preview_menu.png")
    print("wrote", OUT / "preview_over.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
