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

## The subset is a static glyph set

`lvgl.font_load` can only render glyphs that are inside the TTF. Adding or
editing Chinese copy after the subset was built does not add glyphs: the App
still runs, but each new character renders as a tofu box. A `main.lua`-only
upload is the usual cause (the script changes, the TTF does not).

Treat the TTF as source, not a one-time asset:

1. after any copy change, rerun `make_cjk_subset.py`;
2. `check_font_coverage.py` must exit 0 for the final `main.lua`;
3. re-upload the TTF and relaunch. Script-only changes need no restart, but
   glyph changes do need the rebuilt font file.

## Runtime memory: every load is a copy

Each `lvgl.font_load(path, {size = ...})` reads the **whole** TTF from disk into
PSRAM and keeps it for the font's lifetime. Loading one file at 8 sizes holds 8
copies: the reference serial terminal loads about 9 roles, so a 105 KiB subset
costs about 1 MiB. A 1 MiB "full CJK" font would cost 8-9 MiB.

- keep each subset around 100-150 KiB (hard ceiling 200 KiB);
- load only the sizes the UI actually uses;
- `font:set_size(n)` resizes an existing font in place, but it changes the
  shared object, so use it only when every label using that font wants the new
  size.

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
- tofu only on newly added copy -> the subset is stale; rebuild and re-upload
  the TTF (re-uploading `main.lua` alone cannot fix it);
- font path typo -> `font_load` returns nil; fallback font renders ASCII only;
- deleted font before `lvgl.deinit()` -> exit crash; do not delete manually;
- SD font path -> an absolute `/sdcard/...` path is supported on this build
  (the boot code registers the SD root as a font root); if `font_load` returns
  nil, check that the TTF is inside the package and the path matches
  `app_dir`;
- older firmware -> `/sdcard/...` font paths are rejected; keep the TTF on FAT
  and load it with `D:apps/<id>/...`;
- too many characters -> subset >200 KiB; trim copy or split the App.
