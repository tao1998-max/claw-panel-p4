# Poster-Style Visual Layering

A rich-looking panel App is mostly offline art plus a thin runtime composition,
not a game engine. Build it in layers.

## Layers

| Layer | Content | Runtime |
|---|---|---|
| L0 | full-screen scene / key art | 1024x600 JPEG -> decode once -> `set_frame`; optional dithered RGB565 if dark chroma blocks are visible |
| L1 | character, title logo, sigil | cropped PNG -> raw ARGB8888 -> `set_raw_argb8888`, or JPEG if opaque |
| L2 | labels, buttons, chips, hit areas | LVGL containers/labels/buttons; Chinese TTF subset |
| L3 | fire, embers, glow, looping ornament | pre-converted EAF passed as `src_data`, a small `lvgl.lottie` loop, `lvgl.animimg` frame sequence, or a short `lvgl.gif`; local area only |

The poster-like reference screen is: a full-bleed tavern scene (L0), a
character/title logo (L1), arrows/page badge/CTA (L2), and a small fire loop
(L3). The code only stacks them; 90% of the look comes from art and typography.

## Asset pipeline

1. Author or generate the scene at 1024x600 (or larger, then crop).
2. Separate elements that must move or fade; keep the rest baked into L0.
3. Export with the source's alpha preserved:
   - scene -> baseline 4:2:0 JPEG (package fallback at lower quality); use
     dithered raw RGB565 only if dark chroma blocks are visible;
   - sprite -> PNG for authoring, then raw ARGB8888;
   - animation -> GIF/sequence -> EAF with the Espressif converter; a vector
     Lottie JSON can run directly (`lvgl.lottie`), and short PNG/JPEG sequences
     or GIFs can use `lvgl.animimg` / `lvgl.gif` from `D:` (FAT) or `S:` (SD).
4. Run `prepare_sd_assets.py` (or `make_raw_background.py`) to generate:
   - SD high-quality JPEG, or optional dithered RGB565;
   - same-size low-quality package fallback;
   - `sd_manifest.json` with actual byte sizes.
5. Keep the package fallback visually acceptable. It is what users see when the
   card is missing.

## JPEG rules

- baseline only, `yuvj420p`/4:2:0; ffmpeg's 4:4:4/4:2:2 variants were rejected
  by the reference decoder (non-standard sampling factors);
- <=4 MiB and <=1920x1080 per file;
- 1024x600 q85-92 is usually 80-300 KiB; q45-55 fallback is often 15-60 KiB;
- do not use progressive JPEG;
- the decoder's 4:2:0 chroma upsampling can leave coloured blocks in very dark,
  smooth gradients. If that is visible on the panel, try raw RGB565 + dither;
  on the reference weather App the difference was not visible, so JPEG stays
  the default.

## Raw pixels

- RGB565: little-endian, 2 bytes/pixel. `image.load_raw(path, w, h)` returns a
  frame; `img:set_frame(frame)` displays it with no copy.
- ARGB8888: bytes are `B,G,R,A`, 4 bytes/pixel; `set_raw_argb8888(w,h,data)`.
- `scripts/make_raw_background.py` decodes with ffmpeg and applies an 8x8 Bayer
  dither before packing RGB565; this removes banding in dark gradients.
- A full-screen RGB565 frame is 1.23 MiB: keep it on SD and keep only one alive.
- `set_raw_rgb565(w,h,data)` still works when the bytes come from `io.read`;
  `load_raw` avoids the Lua string copy.
- release `image.frame` and converted views immediately after handing them off
  (`set_frame` keeps its own reference).

## Animation

- EAF and Lottie are the primary animation formats. `lvgl.animimg` plays
  PNG/JPEG/bin frame sequences and `lvgl.gif` plays GIFs, but both decode on the
  fly and are CPU-heavy; prefer EAF/Lottie for large or long loops.
- `lvgl.eaf{src_data = bytes}` copies the bytes in C; keep the Lua string and
  the C copy in mind for peak memory. EAF `src` also accepts `D:`/`S:` paths.
- `lvgl.lottie{src = path}` renders vector JSON through ThorVG. Its `src` may
  point at `/sdcard` (unlike EAF), and `src_data` also works. Each instance
  costs `w*h*4` bytes of ARGB8888 (~100 KiB at 160x160) and renders in software.
- `lvgl.gif{src = "D:/..."}` decodes GIF frames into `w*h*4` (ARGB8888) or
  `w*h*2` (RGB565 via `color_format="rgb565"`). It reads `D:` (FAT) or `S:` (SD).
- `lvgl.animimg{src = {...}}` cycles still images from `D:` or `S:`; the source
  array is kept alive by the binding.
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
and one local EAF/Lottie/animimg/gif alive.

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
