# Design System for 1024x600

## Brief

Write these down before drawing:

- purpose and audience;
- tone (calm tool, dense dashboard, game poster, editorial, industrial);
- real copy, including Chinese punctuation;
- required states: idle/running/paused/done, selected/pressed/disabled,
  empty/error;
- hardware constraints: no screenshot, one full-screen bitmap, ~1.7 MiB FAT.

Then write a compact plan: 4-6 color tokens, font roles, an ASCII wireframe with
coordinates, one memorable element, and a sentence explaining why the result is
not a generic template.

## Grid

- canvas: 1024x600, full-screen, no scrolling unless deliberate;
- safe margin: 32-40 px;
- header: 0-100 px;
- content: x=40..984, y=100..576;
- bottom action row: 56-64 px high, buttons at least 44 px tall.

Reference layouts:

- timer: panel 680x472 at (172,104); ring 290 at center (512,300); preset row
  y=462 h=50; control row y=520 h=48; buttons 120/200/120 with 20 px gaps.
- card/game: full-bleed table or scene, fixed sprite sizes (for example
  96x140), status strip at top or bottom, controls outside the play area.

## Tokens (starting point, not a mandate)

```text
bg        #070b14
panel     #111a2b
panel-2   #0c1322
accent    #36d1c4
warning   #ffcf6b
danger    #ff5f6d
text      #f2f6ff
muted     #7f8ca6
button    #1b2740
border    #2c3d5e
```

For a poster-like App, choose a scene palette instead: deep ink, one warm
accent, one metallic gold, and muted paper. Do not use the default purple/cyan
gradient unless the brief asks for it.

## Type

- display/logo: 40-64 px, often an image rather than text;
- title: 34-48 px;
- digits: 72-96 px;
- body: 18-20 px;
- small: 16 px;
- Chinese line height is about 1.44x the font size.

Load one TTF subset and set it as default. Use explicit font handles for display
and small text. Check coverage before uploading.

## Interaction

- touch targets >=44 px;
- leave 8-20 px between rows that can be pressed;
- calculate `y + h` before writing the next row; avoid negative coordinates
  that push content off-screen;
- center with the object option `align="center"`; `text_align` is not
  implemented;
- every interactive state needs a visible pressed/selected/disabled difference;
- provide a clear empty/error state with the next action.

## Anti-template checklist

Reject these unless the brief explicitly calls for them:

- every section is the same rounded card with the same shadow;
- centered title + centered subtitle + centered big number on every screen;
- decorative 01/02/03 labels with no meaning;
- all-uppercase micro-labels everywhere;
- every element fades/slides in;
- emoji used as icons;
- glowing everything with no hierarchy;
- default purple/teal gradient on a dark background.

Keep one memorable element: a strong title, a single oversized number, a
distinctive frame, or a good background. Let the rest be quiet.

## Verification without screenshots

There is no screenshot or pixel-probe API. Use:

1. an ASCII wireframe with coordinates;
2. static checks for overlaps and font coverage;
3. a short serial log of which layers used SD vs fallback;
4. the user's visual confirmation on the real panel.

## App icons

The home grid draws App icons at 96x96 in a six-column phone-style layout. An
icon is not a poster: at that size gradients, shadows and text turn to mud.

- one flat glyph on one gradient background, drawn edge-to-edge;
- no text, no thin outlines, no transparency (baseline JPEG has none);
- 96x96, baseline 4:2:0, roughly 1.5-3 KB;
- keep a single accent hue per App so the grid reads as a set;
- `scripts/make_app_icons.py` has primitives (rounded rect, circle, ring,
  line, polygon) that render this style with ffmpeg.

## Drawing data-dense screens

Charts, sparklines, heat maps and gradient bands are images, not hundreds of
labels. Build them with `image.new()` + `fill_rect`/`line`/`blit` in C and show
the result through `img:set_frame(canvas)`; doing the same with per-pixel Lua
strings costs 10-100x more CPU and PSRAM. Create the canvas once and reuse it:
a 920x320 intraday chart redraws in about 50 ms on the P4, and the old
per-row `string.sub`/`string.concat` builder was the hot spot. Redraw only the
changed region when the data updates.

## Full-screen artwork

A 1024x600 JPEG decoded once and shown with `img:set_frame()` is the default
background path. If a dark gradient shows visible chroma blocks/banding (the
device decoder's 4:2:0 upsampling can do this), an optional dithered raw path
is available:

- generate with `scripts/make_raw_background.py` (ffmpeg decode + 8x8 Bayer),
  load with `image.load_raw(path, 1024, 600)` and show with `img:set_frame()`;
- it removes runtime decode and the Lua string copy, but is 1.23 MiB per frame
  (SD only, one buffer alive);
- keep the 1024x600 JPEG (q45-55 in the package, q85-92 on SD) as fallback;
- on the reference weather App the raw difference was not visible, so it stays
  on JPEG.
