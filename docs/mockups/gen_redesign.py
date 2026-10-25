#!/usr/bin/env python3
"""Redesign preview: dark "aurora console" style, 1024x600.

Outputs:
    docs/mockups/new_settings_display.png
    docs/mockups/new_settings_network.png
    docs/mockups/new_home.png
    docs/mockups/new_sysinfo.png
    docs/mockups/new_overview.png
"""

import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

W, H = 1024, 600
HEADER_H = 64
NAV_H = 68

# ---------------- palette ----------------
BG_TOP = (0x06, 0x0A, 0x14)
BG_BOT = (0x09, 0x11, 0x20)
SURFACE = (0x0E, 0x17, 0x27)
SURFACE_DARK = (0x0B, 0x12, 0x21)
CARD = (0x13, 0x1E, 0x33)
TILE = (0x1A, 0x27, 0x40)
BORDER = (0x24, 0x33, 0x50)
BORDER_SOFT = (0x1A, 0x25, 0x3B)
TEXT = (0xF4, 0xF7, 0xFC)
MUTED = (0x93, 0xA0, 0xB8)
DISABLED = (0x5C, 0x6A, 0x84)
ACCENT = (0x7B, 0x88, 0xFF)
ACCENT_DEEP = (0x6D, 0x76, 0xF5)
VIOLET = (0xA0, 0x7C, 0xFF)
CYAN = (0x4D, 0xD6, 0xCB)
OK = (0x3D, 0xDC, 0x97)
WARN = (0xF6, 0xB8, 0x4C)
ERROR = (0xFF, 0x6B, 0x81)

FONT = "C:/Windows/Fonts/msyh.ttc"
FONT_BOLD = "C:/Windows/Fonts/msyhbd.ttc"
OUT = Path(__file__).resolve().parent


def font(size, bold=False):
    return ImageFont.truetype(FONT_BOLD if bold else FONT, size)


def text(d, xy, s, size=20, color=TEXT, anchor="la", bold=False):
    d.text(xy, s, font=font(size, bold), fill=color, anchor=anchor)


def rrect(d, box, radius=14, fill=None, outline=None, width=1):
    d.rounded_rectangle(box, radius=radius, fill=fill, outline=outline, width=width)


def mix(a, b, t):
    return tuple(int(x * (1 - t) + y * t) for x, y in zip(a, b))


def tint(base, alpha, bg=SURFACE):
    return mix(bg, base, alpha)


def vgrad(img, box, top, bottom, radius):
    """Vertical gradient clipped to a rounded rect."""
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    grad = Image.new("RGB", (1, max(h, 1)))
    for y in range(max(h, 1)):
        grad.putpixel((0, y), mix(top, bottom, y / max(h - 1, 1)))
    grad = grad.resize((w, h))
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, w - 1, h - 1], radius=radius, fill=255)
    img.paste(grad, (x0, y0), mask)


def glow(img, cx, cy, r, color, alpha):
    """Very soft radial glow via concentric alpha circles."""
    overlay = Image.new("RGBA", img.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(overlay)
    steps = 18
    for i in range(steps, 0, -1):
        rr = int(r * i / steps)
        a = int(alpha * (1 - i / steps) ** 1.6)
        d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=color + (a,))
    img.alpha_composite(overlay)


# ---------------- icons (geometry) ----------------

def icon_gear(d, cx, cy, c, w=2):
    import math
    d.ellipse([cx - 5, cy - 5, cx + 5, cy + 5], outline=c, width=w)
    d.ellipse([cx - 2, cy - 2, cx + 2, cy + 2], fill=c)
    for i in range(8):
        a = math.radians(i * 45)
        x0, y0 = cx + math.cos(a) * 6.5, cy + math.sin(a) * 6.5
        x1, y1 = cx + math.cos(a) * 9, cy + math.sin(a) * 9
        d.line([x0, y0, x1, y1], fill=c, width=w)


def icon_eye(d, cx, cy, c, w=2):
    d.arc([cx - 10, cy - 6, cx + 10, cy + 6], 0, 180, fill=c, width=w)
    d.arc([cx - 10, cy - 6, cx + 10, cy + 6], 180, 360, fill=c, width=w)
    d.ellipse([cx - 3, cy - 3, cx + 3, cy + 3], fill=c)


def icon_volume(d, cx, cy, c, w=2):
    d.polygon([(cx - 8, cy - 3), (cx - 4, cy - 3), (cx + 1, cy - 8),
               (cx + 1, cy + 8), (cx - 4, cy + 3), (cx - 8, cy + 3)], fill=c)
    d.arc([cx - 2, cy - 8, cx + 10, cy + 8], -55, 55, fill=c, width=w)
    d.arc([cx - 5, cy - 11, cx + 13, cy + 11], -50, 50, fill=c, width=w)


def icon_wifi(d, cx, cy, c, w=2):
    for r in (9, 5.5, 2):
        d.arc([cx - r, cy - r + 4, cx + r, cy + r + 4], 205, 335, fill=c, width=w)
    d.ellipse([cx - 1.5, cy + 5, cx + 1.5, cy + 8], fill=c)


def icon_home(d, cx, cy, c, w=2):
    d.line([(cx - 9, cy), (cx, cy - 8), (cx + 9, cy)], fill=c, width=w, joint="curve")
    d.rounded_rectangle([cx - 6, cy, cx + 6, cy + 8], radius=1, outline=c, width=w)


def icon_list(d, cx, cy, c, w=2):
    for i in (-6, 0, 6):
        d.ellipse([cx - 9, cy + i - 1.5, cx - 6, cy + i + 1.5], fill=c)
        d.line([cx - 2, cy + i, cx + 9, cy + i], fill=c, width=w)


def icon_clock(d, cx, cy, c, w=2):
    d.ellipse([cx - 9, cy - 9, cx + 9, cy + 9], outline=c, width=w)
    d.line([cx, cy, cx, cy - 5], fill=c, width=w)
    d.line([cx, cy, cx + 4, cy + 2], fill=c, width=w)


def icon_chip(d, cx, cy, c, w=2):
    d.rounded_rectangle([cx - 7, cy - 7, cx + 7, cy + 7], radius=3, outline=c, width=w)
    d.rounded_rectangle([cx - 3, cy - 3, cx + 3, cy + 3], radius=1, fill=c)
    for i in (-4, 0, 4):
        d.line([cx + i, cy - 10, cx + i, cy - 7], fill=c, width=w)
        d.line([cx + i, cy + 7, cx + i, cy + 10], fill=c, width=w)
        d.line([cx - 10, cy + i, cx - 7, cy + i], fill=c, width=w)
        d.line([cx + 7, cy + i, cx + 10, cy + i], fill=c, width=w)


def icon_refresh(d, cx, cy, c, w=2):
    d.arc([cx - 8, cy - 8, cx + 8, cy + 8], 40, 320, fill=c, width=w)
    d.polygon([(cx + 6, cy - 9), (cx + 10, cy - 3), (cx + 3, cy - 3)], fill=c)


def icon_sd(d, cx, cy, c, w=2):
    d.rounded_rectangle([cx - 6, cy - 9, cx + 6, cy + 9], radius=2, outline=c, width=w)
    d.line([cx - 3, cy - 6, cx + 3, cy - 6], fill=c, width=w)
    d.line([cx - 3, cy + 2, cx + 3, cy + 2], fill=c, width=w)


def icon_camera(d, cx, cy, c, w=2):
    d.rounded_rectangle([cx - 9, cy - 6, cx + 9, cy + 7], radius=3, outline=c, width=w)
    d.ellipse([cx - 4, cy - 3, cx + 4, cy + 5], outline=c, width=w)
    d.rounded_rectangle([cx - 3, cy - 9, cx + 3, cy - 6], radius=1, fill=c)


def icon_lock(d, cx, cy, c, w=2):
    d.rounded_rectangle([cx - 6, cy - 1, cx + 6, cy + 8], radius=2, outline=c, width=w)
    d.arc([cx - 4, cy - 8, cx + 4, cy + 2], 180, 360, fill=c, width=w)


def icon_info(d, cx, cy, c, w=2):
    d.ellipse([cx - 9, cy - 9, cx + 9, cy + 9], outline=c, width=w)
    d.ellipse([cx - 1.5, cy - 6, cx + 1.5, cy - 3.5], fill=c)
    d.line([cx, cy - 1, cx, cy + 6], fill=c, width=w)


def icon_check(d, cx, cy, c, w=2):
    d.line([(cx - 8, cy), (cx - 2, cy + 6), (cx + 9, cy - 7)], fill=c, width=w,
           joint="curve")


def icon_file(d, cx, cy, c, w=2):
    d.rounded_rectangle([cx - 7, cy - 9, cx + 7, cy + 9], radius=2,
                        outline=c, width=w)
    d.line([cx - 3, cy - 3, cx + 3, cy - 3], fill=c, width=w)
    d.line([cx - 3, cy + 1, cx + 3, cy + 1], fill=c, width=w)
    d.line([cx - 3, cy + 5, cx + 3, cy + 5], fill=c, width=w)


def icon_wired(d, cx, cy, c, w=2):
    """和 ui_shell.c 里几何拼出来的网口插头保持一致。"""
    d.rounded_rectangle([cx - 6, cy - 9, cx + 6, cy - 1], radius=2,
                        outline=c, width=w)
    for dx in (-3, 0, 3):
        d.rounded_rectangle([cx + dx - 1, cy - 1, cx + dx + 1, cy + 3],
                            radius=1, fill=c)
    d.line([(cx, cy + 3), (cx, cy + 7), (cx + 6, cy + 7)], fill=c, width=w,
           joint="curve")


def icon_bars(d, cx, cy, c, w=3):
    for dx, h in ((-6, 8), (0, 13), (6, 18)):
        d.line([cx + dx, cy + 9, cx + dx, cy + 9 - h], fill=c, width=w)


def icon_tint(d, cx, cy, c, w=2):
    d.polygon([(cx, cy - 10), (cx + 7, cy + 1), (cx - 7, cy + 1)], fill=c)
    d.ellipse([cx - 7, cy - 1, cx + 7, cy + 9], fill=c)


def icon_warning(d, cx, cy, c, w=2):
    d.polygon([(cx, cy - 9), (cx + 9, cy + 7), (cx - 9, cy + 7)], outline=c, width=w)
    d.line([cx, cy - 3, cx, cy + 2], fill=c, width=w)
    d.ellipse([cx - 1.5, cy + 4.5, cx + 1.5, cy + 6], fill=c)


def icon_display(d, cx, cy, c, w=2):
    d.rounded_rectangle([cx - 9, cy - 7, cx + 9, cy + 4], radius=2, outline=c, width=w)
    d.line([cx - 4, cy + 8, cx + 4, cy + 8], fill=c, width=w)
    d.line([cx, cy + 4, cx, cy + 8], fill=c, width=w)


ICONS = {
    "gear": icon_gear, "eye": icon_eye, "volume": icon_volume, "wifi": icon_wifi,
    "home": icon_home, "list": icon_list, "clock": icon_clock, "chip": icon_chip,
    "refresh": icon_refresh, "sd": icon_sd, "camera": icon_camera, "lock": icon_lock,
    "info": icon_info, "warning": icon_warning, "display": icon_display,
    "check": icon_check, "file": icon_file, "wired": icon_wired,
    "bars": icon_bars, "tint": icon_tint,
    "touch": icon_eye, "audio": icon_volume, "rs485": icon_list, "eth": icon_chip,
}


def icon(d, name, cx, cy, color, w=2):
    ICONS[name](d, cx, cy, color, w)


def badge(d, x, y, size, name, color, radius=None, tint_a=0.16, icon_scale=1.0):
    rrect(d, [x, y, x + size, y + size], radius=radius or size // 4,
          fill=tint(color, tint_a), outline=tint(color, min(tint_a + 0.10, 0.5)))
    icon(d, name, x + size // 2, y + size // 2, color,
         w=2 if icon_scale >= 1 else 1)


def chip(d, xy_right, y, label, color, size=16, padx=12, h=28):
    f = font(size)
    tw = d.textlength(label, font=f)
    x0 = xy_right - int(tw) - padx * 2
    rrect(d, [x0, y, xy_right, y + h], radius=h // 2,
          fill=tint(color, 0.14), outline=tint(color, 0.26))
    text(d, (x0 + padx, y + h // 2 - 1), label, size, color, "lm")
    return x0


# ---------------- chrome ----------------

def chrome(d, img, title, active_nav=2, net="wifi"):
    vgrad(img, [0, 0, W, H], BG_TOP, BG_BOT, 0)
    d = ImageDraw.Draw(img)

    # header：透明，整屏 Aurora 背景透出来，只保留一条细分隔线
    d.line([0, HEADER_H - 1, W, HEADER_H - 1], fill=BORDER_SOFT)

    # brand tile with gradient
    tile = Image.new("RGB", (32, 32))
    td = ImageDraw.Draw(tile)
    for y in range(32):
        td.line([0, y, 32, y], fill=mix(ACCENT_DEEP, VIOLET, y / 31))
    mask = Image.new("L", (32, 32), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, 31, 31], radius=10, fill=255)
    img.paste(tile, (24, 16), mask)
    text(d, (40, 32), "P4", 15, (0x0A, 0x0E, 0x1C), "mm", bold=True)

    text(d, (68, HEADER_H // 2), title, 26, TEXT, "lm", bold=True)

    # right: uptime + network icon only（手机状态栏风格）
    text(d, (W - 64, HEADER_H // 2), "运行 00:12:35", 16, MUTED, "rm")
    if net == "wired":
        icon(d, "wired", W - 36, HEADER_H // 2, OK)
    else:
        icon(d, "wifi", W - 36, HEADER_H // 2, CYAN)

    # bottom nav：透明，只保留顶部细分隔线
    d.line([0, H - NAV_H, W, H - NAV_H], fill=BORDER_SOFT)
    items = [("home", "主页"), ("list", "设备状态"), ("bars", "历史曲线"),
             ("gear", "设置")]
    start = (W - 200 * 4) // 2
    for i, (ic, label) in enumerate(items):
        cx = start + i * 200 + 100
        cy = H - NAV_H // 2 + 2
        if i == active_nav:
            rrect(d, [cx - 88, cy - 21, cx + 88, cy + 21], radius=21,
                  fill=tint(ACCENT, 0.13), outline=tint(ACCENT, 0.30))
            d.rounded_rectangle([cx - 24, H - NAV_H, cx + 24, H - NAV_H + 3], radius=2,
                                fill=ACCENT)
        color = ACCENT if i == active_nav else MUTED
        icon(d, ic, cx - 36, cy, color, 2)
        text(d, (cx - 16, cy), label, 18, TEXT if i == active_nav else MUTED, "lm",
             bold=(i == active_nav))


def content_bg(img):
    """Content-area soft glow."""
    glow(img, 850, 40, 260, ACCENT, 22)
    glow(img, 120, 500, 240, VIOLET, 14)


# ---------------- settings ----------------

CATS = [("display", "显示"), ("volume", "声音"), ("wifi", "网络"), ("gear", "系统")]
CAT_COLORS = [ACCENT, OK, CYAN, VIOLET]


def settings_frame(img, active):
    d = ImageDraw.Draw(img)
    chrome(d, img, "设置", 3)
    content_bg(img)
    d = ImageDraw.Draw(img)

    # rail
    rail_x, rail_y, rail_w, rail_h = 24, 88, 272, 420
    vgrad(img, [rail_x, rail_y, rail_x + rail_w, rail_y + rail_h],
          (0x10, 0x1A, 0x2D), (0x0B, 0x13, 0x22), 22)
    d = ImageDraw.Draw(img)
    rrect(d, [rail_x, rail_y, rail_x + rail_w, rail_y + rail_h], radius=22,
          outline=BORDER_SOFT)

    badge(d, rail_x + 20, rail_y + 20, 44, "gear", ACCENT, radius=14, tint_a=0.18)
    text(d, (rail_x + 78, rail_y + 22), "设置中心", 20, TEXT, bold=True)
    text(d, (rail_x + 78, rail_y + 50), "v1.1.0", 14, DISABLED)
    d.line([rail_x + 20, rail_y + 84, rail_x + rail_w - 20, rail_y + 84], fill=BORDER_SOFT)

    for i, (ic, label) in enumerate(CATS):
        y = rail_y + 98 + i * 60
        on = i == active
        if on:
            vgrad(img, [rail_x + 12, y, rail_x + rail_w - 12, y + 52],
                  tint(ACCENT, 0.24, SURFACE), tint(ACCENT, 0.06, SURFACE), 14)
            d = ImageDraw.Draw(img)
            rrect(d, [rail_x + 12, y, rail_x + rail_w - 12, y + 52], radius=14,
                  outline=tint(ACCENT, 0.32))
            d.rounded_rectangle([rail_x + 20, y + 15, rail_x + 23, y + 37], radius=1,
                                fill=ACCENT)
        badge(d, rail_x + 34, y + 10, 32, ic, CAT_COLORS[i],
              radius=10, tint_a=0.20 if on else 0.10)
        text(d, (rail_x + 78, y + 26), label, 18,
             TEXT if on else MUTED, "lm", bold=on)
        if not on:
            text(d, (rail_x + rail_w - 28, y + 26), "›", 18, DISABLED, "mm")

    # detail panel
    dx, dy, dw, dh = 312, 88, 688, 420
    vgrad(img, [dx, dy, dx + dw, dy + dh], (0x0F, 0x18, 0x2A), (0x0C, 0x14, 0x24), 22)
    d = ImageDraw.Draw(img)
    rrect(d, [dx, dy, dx + dw, dy + dh], radius=22, outline=BORDER_SOFT)

    # decorative index
    text(d, (dx + dw - 28, dy + 32), f"0{active + 1}", 40, tint(ACCENT, 0.18, SURFACE),
         "ra", bold=True)

    return d, (dx, dy, dw, dh)


def settings_header(d, box, title, subtitle, color, ic):
    dx, dy, dw, dh = box
    badge(d, dx + 24, dy + 24, 48, ic, color, radius=15, tint_a=0.18)
    text(d, (dx + 88, dy + 26), title, 26, TEXT, bold=True)
    text(d, (dx + 88, dy + 62), subtitle, 14, DISABLED)
    d.line([dx + 24, dy + 96, dx + dw - 24, dy + 96], fill=BORDER_SOFT)
    return dx + 24, dy + 110, dw - 48, dh - 134  # body x,y,w,h


def slider(d, x, y, w, ratio, h=10):
    d.rounded_rectangle([x, y, x + w, y + h], radius=h // 2, fill=TILE)
    d.rounded_rectangle([x, y, x + int(w * ratio), y + h], radius=h // 2, fill=ACCENT)
    kx = x + int(w * ratio)
    d.ellipse([kx - 9, y + h // 2 - 9, kx + 9, y + h // 2 + 9], fill=TEXT,
              outline=ACCENT, width=3)


def segmented(img, d, x, y, w, h, labels, active, size=16):
    rrect(d, [x, y, x + w, y + h], radius=h // 2, fill=(0x10, 0x19, 0x2B),
          outline=BORDER_SOFT)
    n = len(labels)
    pad = 4
    gap = 3
    iw = (w - pad * 2 - (n - 1) * gap) // n
    for i, s in enumerate(labels):
        bx = x + pad + i * (iw + gap)
        bw = iw if i < n - 1 else (x + w - pad - bx)
        on = i == active
        if on:
            vgrad(img, [bx, y + pad, bx + bw, y + h - pad],
                  ACCENT_DEEP, VIOLET, (h - pad * 2) // 2)
            dd = ImageDraw.Draw(img)
            dd.rounded_rectangle([bx, y + pad, bx + bw, y + h - pad],
                                 radius=(h - pad * 2) // 2,
                                 outline=tint(ACCENT, 0.55))
            text(dd, (bx + bw // 2, y + h // 2), s, size, (0x0B, 0x0F, 0x1D), "mm",
                 bold=True)
        else:
            text(d, (bx + bw // 2, y + h // 2), s, size, MUTED, "mm")


def card(d, x, y, w, h, fill=CARD):
    rrect(d, [x, y, x + w, y + h], radius=17, fill=fill, outline=BORDER_SOFT)


def settings_display(img):
    d, box = settings_frame(img, 0)
    bx, by, bw, bh = settings_header(d, box, "显示", "亮度、息屏与显示行为", ACCENT, "display")

    # brightness（只保留标题 + 数值 + 滑条）
    card(d, bx, by, bw, 92)
    text(d, (bx + 20, by + 16), "显示亮度", 18, TEXT)
    text(d, (bx + bw - 20, by + 12), "80%", 26, ACCENT, "ra", bold=True)
    slider(d, bx + 20, by + 58, bw - 40, 0.8)

    # screen off
    card(d, bx, by + 108, bw, 148)
    text(d, (bx + 20, by + 124), "息屏时间", 18, TEXT)
    chip(d, bx + bw - 20, by + 118, "5 分钟", ACCENT, 13, 10, 26)
    segmented(img, d, bx + 20, by + 162, bw - 40, 48, ["常亮", "30 秒", "1 分钟", "5 分钟"], 3)
    text(d, (bx + 20, by + 222), "无操作到时自动关闭背光，触摸即可点亮", 13, DISABLED)
    return img


def settings_sound(img):
    d, box = settings_frame(img, 1)
    bx, by, bw, bh = settings_header(d, box, "声音", "音量控制", OK, "volume")

    card(d, bx, by, bw, 92)
    text(d, (bx + 20, by + 16), "扬声器音量", 18, TEXT)
    text(d, (bx + bw - 20, by + 12), "70%", 26, OK, "ra", bold=True)
    slider(d, bx + 20, by + 58, bw - 40, 0.7)
    return img


def settings_network(img):
    d, box = settings_frame(img, 2)
    bx, by, bw, bh = settings_header(d, box, "网络", "Wi-Fi 连接与网络状态", CYAN, "wifi")

    # status card
    card(d, bx, by, bw, 108)
    badge(d, bx + 20, by + 18, 40, "wifi", CYAN, radius=13)
    text(d, (bx + 72, by + 14), "Wi-Fi", 18, TEXT)
    text(d, (bx + 72, by + 42), "CU_k3eY", 18, TEXT, bold=True)
    d.ellipse([bx + 74, by + 79, bx + 82, by + 87], fill=OK)
    text(d, (bx + 88, by + 72), "已连接", 15, OK)
    text(d, (bx + 190, by + 72), "192.168.1.179", 14, MUTED)

    rrect(d, [bx + 348, by + 18, bx + 476, by + 54], radius=13,
          fill=TILE, outline=BORDER)
    text(d, (bx + 412, by + 36), "断开", 14, MUTED, "mm")
    rrect(d, [bx + 484, by + 18, bx + bw - 20, by + 54], radius=13,
          fill=ACCENT, outline=None)
    text(d, (bx + 552, by + 36), "扫描网络", 14, (0x0B, 0x0F, 0x1D), "mm", bold=True)

    # list heading
    text(d, (bx, by + 118), "可用网络", 16, MUTED)

    # 列表区域 146 px，恰好放 3 行；更多 AP 靠滚动
    aps = [("CU_k3eY", -42, True), ("ChinaNet-8F2", -58, False),
           ("TP-LINK_5G", -66, False)]
    for i, (ssid, rssi, cur) in enumerate(aps):
        y = by + 140 + i * 51
        rrect(d, [bx, y, bx + bw, y + 44], radius=13, fill=CARD,
              outline=tint(CYAN, 0.22) if cur else BORDER_SOFT)
        icon(d, "wifi", bx + 22, y + 22, OK if cur else CYAN, 1)
        text(d, (bx + 50, y + 22), ssid, 16, TEXT if cur else MUTED, "lm",
             bold=cur)
        text(d, (bx + bw - 20, y + 22), f"{rssi} dBm", 13, DISABLED, "rm")
    return img


def settings_system(img):
    d, box = settings_frame(img, 3)
    bx, by, bw, bh = settings_header(d, box, "系统", "语言、版本与恢复出厂", VIOLET, "gear")

    card(d, bx, by, bw, 112)
    text(d, (bx + 20, by + 16), "语言", 18, TEXT)
    segmented(img, d, bx + 20, by + 50, bw - 40, 46, ["中文", "English"], 0)

    card(d, bx, by + 122, bw, 70)
    badge(d, bx + 18, by + 141, 32, "info", VIOLET, radius=10, tint_a=0.14)
    text(d, (bx + 62, by + 136), "固件规格", 16, TEXT)
    text(d, (bx + 62, by + 158), "v1.1.0  ·  JC1060P470C_I_W_Y  ·  P4 + C6", 13, DISABLED)

    # danger
    rrect(d, [bx, by + 202, bx + bw, by + 202 + 86], radius=17,
          fill=tint(ERROR, 0.07, CARD), outline=tint(ERROR, 0.22))
    badge(d, bx + 18, by + 221, 40, "warning", ERROR, radius=12, tint_a=0.15)
    text(d, (bx + 70, by + 217), "恢复出厂设置", 17, ERROR)
    text(d, (bx + 70, by + 241), "清空全部参数并重启设备", 13, DISABLED)
    rrect(d, [bx + bw - 158, by + 223, bx + bw - 20, by + 265], radius=13,
          fill=tint(ERROR, 0.14), outline=tint(ERROR, 0.35))
    text(d, (bx + bw - 89, by + 244), "恢复出厂", 14, ERROR, "mm")
    return img


# ---------------- home ----------------

def home(img):
    d = ImageDraw.Draw(img)
    chrome(d, img, "主页", 0)
    content_bg(img)
    d = ImageDraw.Draw(img)

    # 主页暂时留空：只有一枚淡图标 + 一行提示
    icon(d, "file", W // 2, H // 2 - 18, DISABLED, 2)
    text(d, (W // 2, H // 2 + 24), "业务页面在 ui/pages/ 下添加", 16, DISABLED, "mm")
    return img


# ---------------- sysinfo ----------------

def sysinfo(img):
    d = ImageDraw.Draw(img)
    chrome(d, img, "设备状态", 1, net="wired")
    content_bg(img)
    d = ImageDraw.Draw(img)

    kpis = [
        ("check", ACCENT, "就绪模块", "8 / 8", 1.0, ACCENT),
        ("chip", CYAN, "内部内存", "128 KB", 0.42, CYAN),
        ("sd", OK, "PSRAM 空闲", "7.2 MB", 0.63, OK),
        ("refresh", VIOLET, "运行时长", "00:12:35", 0.0, VIOLET),
    ]
    cw, cg, chh = 232, 16, 112
    for i, (ic, color, caption, value, ratio, bar_color) in enumerate(kpis):
        cx = 24 + i * (cw + cg)
        cy = 88
        vgrad(img, [cx, cy, cx + cw, cy + chh], (0x12, 0x1C, 0x31), (0x0E, 0x16, 0x27), 18)
        d = ImageDraw.Draw(img)
        rrect(d, [cx, cy, cx + cw, cy + chh], radius=18, outline=BORDER_SOFT)
        badge(d, cx + cw - 52, cy + 16, 34, ic, color, radius=10, tint_a=0.13)
        text(d, (cx + 20, cy + 16), caption, 13, MUTED)
        text(d, (cx + 20, cy + 40), value, 26, TEXT, bold=True)
        if ratio > 0:
            d.rounded_rectangle([cx + 20, cy + 96, cx + cw - 20, cy + 100], radius=2, fill=TILE)
            d.rounded_rectangle([cx + 20, cy + 96, cx + 20 + int((cw - 40) * ratio), cy + 100],
                                radius=2, fill=bar_color)

    # panel
    py, ph = 216, 292
    vgrad(img, [24, py, 1000, py + ph], (0x0F, 0x18, 0x2A), (0x0C, 0x14, 0x24), 22)
    d = ImageDraw.Draw(img)
    rrect(d, [24, py, 1000, py + ph], radius=22, outline=BORDER_SOFT)
    text(d, (48, py + 18), "模块状态", 18, TEXT, bold=True)
    text(d, (976, py + 20), "● 就绪      ● 异常      ● 未启用", 12, DISABLED, "ra")

    mods = [
        ("display", "显示", "就绪", OK), ("touch", "触摸", "就绪", OK),
        ("audio", "音频", "就绪", OK), ("sd", "SD 卡", "未启用", DISABLED),
        ("rs485", "RS485", "就绪", OK), ("eth", "以太网", "就绪", OK),
        ("camera", "摄像头", "就绪", OK), ("wifi", "Wi-Fi", "就绪", OK),
    ]
    cell_w, cell_h, gx, gy = 468, 47, 8, 6
    for i, (ic, name, state, color) in enumerate(mods):
        col, row = i % 2, i // 2
        x = 24 + 16 + col * (cell_w + gx)
        y = py + 54 + row * (cell_h + gy)
        rrect(d, [x, y, x + cell_w, y + cell_h], radius=12, fill=CARD,
              outline=BORDER_SOFT)
        badge(d, x + 12, y + 9, 28, ic, color, radius=9, tint_a=0.12)
        text(d, (x + 52, y + cell_h // 2), name, 16, TEXT, "lm")
        d.ellipse([x + cell_w - 92, y + cell_h // 2 - 4, x + cell_w - 84,
                   y + cell_h // 2 + 4], fill=color)
        text(d, (x + cell_w - 76, y + cell_h // 2), state, 13, color, "lm")

    text(d, (48, py + ph - 24), "固件 v1.1.0   ·   JC1060P470C_I_W_Y   ·   MAC 1A:2B:3C:4D:5E:6F",
         12, DISABLED)
    return img


# ---------------- history ----------------

def history(img):
    d = ImageDraw.Draw(img)
    chrome(d, img, "历史曲线", 2, net="wired")
    content_bg(img)
    d = ImageDraw.Draw(img)

    x, y, w, h = 24, 88, 976, 420
    vgrad(img, [x, y, x + w, y + h], (0x10, 0x1A, 0x2D),
          (0x0B, 0x13, 0x22), 22)
    d = ImageDraw.Draw(img)
    rrect(d, [x, y, x + w, y + h], radius=22, outline=BORDER_SOFT)

    # 一行头部：标题 + 当前值 + 缩放胶囊 + 系列下拉 + 区段查找
    badge(d, x + 20, y + 11, 32, "tint", WARN, radius=11, tint_a=0.16)
    text(d, (x + 62, y + 13), "历史曲线", 20, TEXT, bold=True)
    text(d, (x + 160, y + 7), "26.4 ℃", 26, WARN, bold=True)

    # 缩放胶囊：-  40s  +（中间数值可点，直接输入时/分/秒）
    zx, zy, zw, zh = x + 376, y + 13, 190, 28
    rrect(d, [zx, zy, zx + zw, zy + zh], radius=14, fill=TILE, outline=BORDER)
    text(d, (zx + 20, zy + 14), "-", 18, TEXT, "mm")
    text(d, (zx + 95, zy + 14), "40s", 13, MUTED, "mm")
    text(d, (zx + 170, zy + 14), "+", 18, TEXT, "mm")

    dd_x, dd_y, dd_w, dd_h = x + 586, y + 8, 180, 38
    rrect(d, [dd_x, dd_y, dd_x + dd_w, dd_y + dd_h], radius=12,
          fill=TILE, outline=BORDER)
    text(d, (dd_x + 14, dd_y + dd_h // 2), "温度", 14, TEXT, "lm")
    d.line([(dd_x + dd_w - 34, dd_y + 15), (dd_x + dd_w - 28, dd_y + 21),
            (dd_x + dd_w - 22, dd_y + 15)], fill=MUTED, width=2)

    # 区段查找入口
    rg_x, rg_y, rg_w, rg_h = x + 776, y + 8, 180, 38
    rrect(d, [rg_x, rg_y, rg_x + rg_w, rg_y + rg_h], radius=12,
          fill=TILE, outline=BORDER)
    text(d, (rg_x + rg_w // 2, rg_y + rg_h // 2), "区段查找", 13, MUTED, "mm")

    d.line([x + 20, y + 54, x + w - 20, y + 54], fill=BORDER_SOFT)

    # 坐标区：X 轴显示日期时间
    px, py, pw, ph = x + 80, y + 66, 876, 322
    yvals = [35.0, 30.0, 25.0, 20.0, 15.0]
    for i, v in enumerate(yvals):
        gy = py + i * ph // 4
        text(d, (x + 72, gy - 8), f"{v:.1f}", 13, MUTED, "ra")
        d.line([px, gy, px + pw, gy],
               fill=BORDER if i == 4 else BORDER_SOFT)
        gx = px + i * pw // 4
        d.line([gx, py, gx, py + ph],
               fill=BORDER if i == 0 else BORDER_SOFT)

    xlabels = ["10-24 19:30", "10-24 19:40", "10-24 19:50",
               "10-24 20:00", "10-24 20:10"]
    for i, lab in enumerate(xlabels):
        gx = px + i * pw // 4
        anchor = "la" if i == 0 else ("ra" if i == 4 else "ma")
        text(d, (gx, py + ph + 12), lab, 12, DISABLED, anchor)

    # 绘图区右上角日期 + 左上角游标读数
    text(d, (px + pw, py + 8), "2026-10-24", 12, DISABLED, "ra")
    text(d, (px + 8, py + 8), "10-24 19:47  20.7 ℃", 12, WARN, "la")

    pts = []
    vals = []
    for i in range(40):
        v = 25.0 + 4.5 * math.sin(i / 40 * 2 * math.pi + 0.5)
        vals.append(v)
        cx = px + i * pw // 39
        cy = py + ph - int((v - 15.0) / 20.0 * ph)
        pts.append((cx, cy))
    d.line(pts, fill=WARN, width=2, joint="curve")

    ci = 25
    cx, cy = pts[ci]
    d.line([cx, py, cx, py + ph], fill=(0x9A, 0xA4, 0xB8))
    d.ellipse([cx - 5, cy - 5, cx + 5, cy + 5], fill=WARN,
              outline=TEXT, width=2)
    return img


def overview(images, cols=3):
    rows = (len(images) + cols - 1) // cols
    sheet = Image.new("RGB", (W * cols + 8 * (cols + 1), H * rows + 8 * (rows + 1)),
                      (0x04, 0x06, 0x0C))
    for i, im in enumerate(images):
        sheet.paste(im, ((i % cols) * (W + 8) + 8, (i // cols) * (H + 8) + 8))
    return sheet.resize((sheet.width // 2, sheet.height // 2), Image.LANCZOS)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    makers = [
        ("new_settings_display", settings_display),
        ("new_settings_sound", settings_sound),
        ("new_settings_network", settings_network),
        ("new_settings_system", settings_system),
        ("new_home", home),
        ("new_sysinfo", sysinfo),
        ("new_history", history),
    ]
    images = []
    for name, fn in makers:
        img = Image.new("RGBA", (W, H), BG_TOP + (255,))
        img = fn(img)
        rgb = img.convert("RGB")
        rgb.save(OUT / f"{name}.png")
        images.append(rgb)
        print("saved", OUT / f"{name}.png")

    overview(images).save(OUT / "new_overview.png")
    print("saved", OUT / "new_overview.png")


if __name__ == "__main__":
    main()
