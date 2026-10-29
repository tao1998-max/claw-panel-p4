# Poster-Style Visual Layering

A rich-looking panel App is mostly offline art plus a thin runtime composition,
not a game engine. Build it in layers.

## Layers

| Layer | Content | Runtime |
|---|---|---|
| L0 | full-screen scene / key art | 1024x600 baseline JPEG -> RGB565 -> `set_raw_rgb565` |
| L1 | character, title logo, sigil | cropped PNG -> raw ARGB8888 -> `set_raw_argb8888`, or JPEG if opaque |
| L2 | labels, buttons, chips, hit areas | LVGL containers/labels/buttons; Chinese TTF subset |
| L3 | fire, embers, glow, looping ornament | pre-converted EAF passed as `src_data`; local area only |

The poster-like reference screen is: a full-bleed tavern scene (L0), a
character/title logo (L1), arrows/page badge/CTA (L2), and a small fire loop
(L3). The code only stacks them; 90% of the look comes from art and typography.

## Asset pipeline

1. Author or generate the scene at 1024x600 (or larger, then crop).
2. Separate elements that must move or fade; keep the rest baked into L0.
3. Export with the source's alpha preserved:
   - scene -> JPEG (baseline, 4:2:0);
   - sprite -> PNG for authoring, then raw ARGB8888;
   - animation -> GIF/sequence -> EAF with the Espressif converter.
4. Run `prepare_sd_assets.py` to generate:
   - SD high-quality JPEG or raw pixels;
   - same-size low-quality package fallback;
   - `sd_manifest.json` with actual byte sizes.
5. Keep the package fallback visually acceptable. It is what users see when the
   card is missing.

## JPEG rules

- baseline only, `yuvj420p`/4:2:0; 4:4:4 failed on the reference device;
- <=4 MiB and <=1920x1080 per file;
- 1024x600 q85-92 is usually 80-300 KiB; q45-55 fallback is often 15-60 KiB;
- do not use progressive JPEG.

## Raw pixels

- RGB565: little-endian, 2 bytes/pixel; `set_raw_rgb565(w,h,data)`.
- ARGB8888: bytes are `B,G,R,A`, 4 bytes/pixel; `set_raw_argb8888(w,h,data)`.
- crop sprites to their bounding box; a full-screen ARGB frame is 2.46 MiB.
- release `image.frame` and converted views immediately after copying data.

## Animation

- EAF is the animation format; GIF is not decoded at runtime.
- `lvgl.eaf{src_data = bytes}` copies the bytes in C; keep the Lua string and
  the C copy in mind for peak memory.
- 33-100 ms per frame is typical; a 30 fps full-screen animation is not viable.
- Animate small regions (fire, embers, a glow) or move/fade a sprite; do not
  re-upload a 1.23 MiB background every frame.

## Memory budget

| Item | Bytes |
|---|---:|
| 1024x600 RGB565 | 1,228,800 |
| 1024x600 ARGB8888 | 2,457,600 |
| 180x180 ARGB8888 | 129,600 |
| 1024x600 JPEG (q90) | ~20-300 KiB |

PSRAM is plentiful compared with internal RAM, but software blending and DSI
bandwidth are the practical limits. Keep one full-screen image, a few sprites,
and one local EAF alive.

## Example structure

`assets/examples/sd_fallback/` demonstrates the contract:

```text
app/
├── launcher.json
├── scripts/main.lua
├── sd_manifest.json
└── assets/
    ├── ui_cn.ttf
    ├── img/bg_fallback.jpg       # same 1024x600
    ├── img/sigil_fallback.jpg    # same 180x180
    └── sfx/click.ogg
sd/panel-assets/sd_fallback/img/
    ├── bg.jpg                    # high quality
    └── sigil.argb                # transparent
```

Launch it without SD assets: it logs `bg=fat sigil=fat` and still renders.
Upload the SD tree and relaunch: it logs `bg=sd sigil=sd`.
