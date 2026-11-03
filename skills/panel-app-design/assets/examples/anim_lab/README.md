# anim_lab: small Lottie + animimg demo

Two 160x160 animation tiles on one screen:

- **LOTTIE** — `lvgl.lottie` with a ring + orbiting dot. The `src` is an
  absolute path (ThorVG uses `fopen`): `/sdcard/panel-assets/anim_lab/anim/loader.json`
  when the SD upgrade is present, otherwise the package copy
  `<app_dir>/assets/anim/loader.json`.
- **ANIMIMG** — `lvgl.animimg` pulsing rings. The SD upgrade has 12 frames
  (`S:/panel-assets/anim_lab/anim/f0..f11.png`); the package fallback has 6
  (`D:apps/anim_lab/assets/anim/f0..f5.png`, or `S:/apps/...` when the package
  itself lives on SD). The App checks every SD frame before switching.
- A pause button calls `lottie:pause()/resume()` and `animimg:stop()/start()`.
- The header badge and card details show whether SD or package assets are in
  use, so the fallback is visible without a screenshot.

## Layout

```text
anim_lab/
├── app/                              # FAT (or SD) package
│   ├── launcher.json
│   ├── scripts/main.lua
│   └── assets/
│       ├── icon.jpg                  # 96x96 baseline JPEG
│       ├── ui_cn.ttf                 # subset for the Chinese copy
│       └── anim/
│           ├── loader.json           # package Lottie (one dot)
│           └── f0..f5.png            # 6 fallback frames
├── sd/panel-assets/anim_lab/anim/    # optional SD upgrade
│   ├── loader.json                   # two dots
│   └── f0..f11.png                   # 12 frames (smoother)
└── source/gen_assets.py              # regenerates frames, Lottie, icon
```

## Build and deploy

```bash
# 1) fonts: Noto Sans SC instanced at wght 500, then the documented subsetter
python scripts/make_cjk_subset.py \
  --source-font NotoSansSC-Medium.ttf \
  --lua assets/examples/anim_lab/app/scripts/main.lua \
  --output assets/examples/anim_lab/app/assets/ui_cn.ttf
python scripts/check_font_coverage.py \
  --ttf assets/examples/anim_lab/app/assets/ui_cn.ttf \
  --lua assets/examples/anim_lab/app/scripts/main.lua

# 2) optional: regenerate the animation assets (Python stdlib + ffmpeg)
python assets/examples/anim_lab/source/gen_assets.py

# 3) deploy the package, then the SD upgrade
python scripts/upload_app.py --host http://<device-ip> \
  --app-dir assets/examples/anim_lab/app --remote-id anim_lab
python scripts/upload_sd_assets.py --host http://<device-ip> \
  --sd-dir assets/examples/anim_lab/sd
```

`NotoSansSC-Medium.ttf` can be produced from `NotoSansSC-VF.ttf` with
`fontTools.varLib.instancer` (`wght=500`); any static CJK TTF works.

## What to verify

1. Launch **without** the SD tree: the badge says `包内`, Lottie still plays,
   animimg shows `6 帧`, and the log prints
   `[anim_lab] lottie=true/90 frames, animimg=true/6 frames, sd_lottie=false sd_frames=false`.
2. Upload the SD tree and relaunch: badge says `SD`, animimg shows `12 帧`,
   the log ends with `sd_lottie=true sd_frames=true`.
3. Delete one SD frame and relaunch: the App falls back to the 6 package
   frames instead of breaking.
4. Tap 暂停/继续: both tiles stop and resume together.
