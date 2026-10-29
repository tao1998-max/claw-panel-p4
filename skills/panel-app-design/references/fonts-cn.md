# Chinese Font Subsets

## Where fonts live

`lvgl.font_load` resolves three forms:

- relative to the Claw data root (`fonts/ui_cn.ttf` -> `/fatfs/claw/data/fonts/...`);
- `D:apps/<id>/assets/ui_cn.ttf` (drive letter mapped to the data root);
- an absolute path under the SD mount (`/sdcard/apps/<id>/assets/ui_cn.ttf`).

So a bundled TTF can live in a FAT package or an SD package; older firmware
rejects absolute `/sdcard` paths and needs the `D:` form with the font on FAT.
The device chat font is not a general-purpose App font; bundle your own subset.

## Source font

Use a redistributable CJK font:

- Noto Serif SC / Noto Sans SC (OFL);
- Source Han Serif/Sans (OFL);
- another font whose license permits embedding.

Keep the license notice next to the example (`OFL.txt` or `NOTICE`). Windows
`deng.ttf`/`msyh.ttc` may be usable locally but check redistribution terms.

## Build a subset

`make_cjk_subset.py` scans `.lua`/text files, includes printable ASCII, and
writes a TTF:

```bash
python scripts/make_cjk_subset.py \
  --source-font NotoSerifSC-Medium.ttf \
  --lua app/scripts/main.lua \
  --output app/assets/ui_cn.ttf
```

Requirements: `pip install fonttools`. The script imports it lazily, so `--help`
works without it.

The reference `countdown_cn.ttf` is ~116 KiB and `ui_cn.ttf` is ~105 KiB for
~120-140 characters. Aim for <200 KiB. If a subset is much larger, the source
font has too much layout/name data or you included too many characters.

## Check coverage

```bash
python scripts/check_font_coverage.py \
  --ttf app/assets/ui_cn.ttf \
  --lua app/scripts/main.lua
```

It reports missing code points and exits 1. Always rerun after changing copy.
Do not "verify" by rendering and looking for boxes; there is no screenshot API.

## What to include

- every visible Chinese character in labels, buttons, hints, status text, and
  error/empty states;
- Chinese punctuation: `，。！？：；、“”‘’（）《》·…—`;
- ASCII letters/digits and spaces if the UI shows them;
- no control characters, CR/LF, or tabs.

Dynamic strings (numbers, counters) need their digits in the subset; the ASCII
range covers them.

## Common failures

- missing glyph -> tofu box; run coverage check;
- font path typo -> `font_load` returns nil; fallback font renders ASCII only;
- deleted font before `lvgl.deinit()` -> exit crash; do not delete manually;
- SD font path -> rejected; move the TTF into the package;
- too many characters -> subset >200 KiB; trim copy or split the App.
