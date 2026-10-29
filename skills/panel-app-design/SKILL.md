---
name: panel-app-design
description: Design, validate, and deliver LVGL Lua apps for the 1024x600 ESP32-P4 panel. Use when creating or redesigning a launcher App, including Chinese UI, poster-style layered visuals, SD-card or FAT app packages, SD fallback, networking (TCP/UDP/HTTP, MQTT, WebSocket, Lua HTTP routes), and Web upload. Do not use for firmware/native drivers, device-side ESP-Claw skills, or generic Lua scripts.
---

# Panel App Design

Create host-side App packages for the 1024x600 ESP32-P4 panel. A package is
ordinary Lua plus assets; the launcher starts it from the FAT workspace
(`/fatfs/claw/data/apps/<id>/`) or, on this firmware build, in place from the SD
card (`/sdcard/apps/<id>/`). The SD card also supplies optional
high-resolution/extra assets under `/sdcard/panel-assets/<id>/`.

## Scope

Use this skill when the task is a **launcher App** for this panel:

- a new App, a redesign, or a visual/game-like screen;
- Chinese UI with a bundled TTF subset;
- images, sprites, animations, or audio;
- FAT or SD package plus optional SD assets and fallback;
- validation and LAN upload;
- networking: TCP/UDP/DNS/HTTP clients, MQTT, WebSocket, and Lua HTTP routes.

Do not use it for firmware/driver work, for the device-side ESP-Claw Agent
Skill format, or for writing a standalone Lua snippet that is not an App.
External AI platforms cannot call the device tools `publish_app`, `read_file`,
or `lua_run_script`; use a local editor and the Web API instead.

## Storage contract

| Tier | Location | Contents | Rules |
|---|---|---|---|
| FAT package | `/fatfs/claw/data/apps/<id>/` | `launcher.json`, `scripts/*.lua`, TTF subset, low-quality fallback images, tiny SFX | Target <=300 KiB; warn >512 KiB; hard-fail >1 MiB unless explicitly allowed; scanned first and overrides an SD package with the same `id` |
| SD package (optional) | `/sdcard/apps/<id>/` | Full package: `launcher.json`, scripts, images, SFX, TTF | Launched in place, read-only; card inserted before boot; restart to rescan; upload limit 64 MiB, no FAT budget |
| SD library (optional) | `/sdcard/panel-assets/<id>/` | high-res JPEG, raw RGB565/ARGB8888, EAF, extra OGG | Loaded only when present; app must work without it |
| Never on SD | — | LVGL `image.src`/`eaf.src` resources | Those resolve against the FAT workspace only; use `image.load_file()` / EAF `src_data` for SD |

`lvgl.font_load()` accepts an absolute `/sdcard/...` path in this firmware
build, so an SD package may keep its TTF inside the package. On older firmware
keep the TTF on FAT and load it via `D:apps/<id>/...`.

Resolution order for every optional asset:

1. try `/sdcard/panel-assets/<id>/...`;
2. on missing file, decode/load failure, or no card, use the same-size package
   fallback under `assets/`;
3. if that also fails, draw a programmatic background/label. Never leave a
   blank screen.

Important consequences:

- SD is mounted once at boot (no hot-plug). Insert the card before power-on.
- `max_files=5` on the SD volume: keep one full-screen image at a time and
  release `image.frame`/converted views immediately.
- There is no runtime image scaling API. The SD image and its FAT fallback must
  have the **same width and height**; only quality changes.
- FAT is tight (about 1.4 MiB free on the reference device). Do not put
  high-res art or EAF in the package.
- `image.load_file()` is baseline JPEG only (<=4 MiB, <=1920x1080). Source
  PNG/WebP/GIF must be converted offline.
- SD file names should be ASCII (`img/bg.jpg`, `sfx/click.ogg`); the card is
  formatted CP936 while Web/scripts use UTF-8.

## Decision tree

1. **Does the screen need custom art?**
   - No: LVGL containers/labels/buttons/lines/arcs are enough.
   - Yes: continue.
2. **Is the asset needed to boot or read the UI?**
   - Yes: put a same-size low-quality fallback in `assets/`; SD is an upgrade.
   - No: SD-only is fine.
3. **What kind of art?**
   - full-screen scene -> 1024x600 baseline JPEG;
   - transparent character/sigil -> raw ARGB8888 (crop to its bounding box);
   - small opaque rectangle -> JPEG;
   - fire/glow/loop -> pre-converted EAF, never GIF at runtime.
4. **Chinese text?** Collect every visible character, build a TTF subset, then
   check coverage. Never rely on the device's chat font.
5. **Which volume?** FAT when the App must start without a card (FAT is scanned
   first and overrides a duplicate id on SD); SD when the package is large or
   FAT is tight — with no card inserted before boot, SD Apps are simply absent.
   Then deliver: `upload_app.py --volume sd` (or copy the package to
   `/sdcard/apps/<id>/`), restart, verify the fallback first, then upload SD
   assets to `panel-assets/` and relaunch.

## Design brief before code

Write a short brief: purpose, audience, tone, real copy, required states, and
hardware constraints. Then write a compact plan:

- 4-6 color tokens and their roles;
- font roles (display/body/small) and sizes;
- an ASCII wireframe with coordinates;
- one memorable element; everything else stays quiet;
- a check that the result is not the generic "centered title + rounded cards +
  purple gradient" default.

Use [`references/design-system.md`](references/design-system.md) for the grid,
type scale, states, and anti-template checklist.

## Poster-style layers

Most "game-like" screens are offline art plus a thin runtime composition:

- **L0** full-screen background: 1024x600 JPEG, q85-92 on SD, q45-55 fallback.
- **L1** transparent sprite/title: PNG -> raw ARGB8888 (`B,G,R,A`); crop to the
  sprite rectangle, not the whole screen.
- **L2** LVGL UI: labels, buttons, status chips, hit areas; text uses the FAT
  TTF subset.
- **L3** local EAF loop for fire/embers/glow; do not animate the full screen.
- **Poster logic**: title/big logo as image, small UI as text, one accent.
  A full-screen RGB565 frame is 1.23 MiB and ARGB8888 is 2.46 MiB; keep only one
  full-screen bitmap alive and avoid full-screen per-frame uploads.

See [`references/visual-layering.md`](references/visual-layering.md).

## Package layout

```text
<id>/
├── launcher.json          # <=4096 bytes, written last
├── scripts/
│   └── main.lua           # <=64 KiB
├── assets/
│   ├── ui_cn.ttf          # bundled CJK subset
│   ├── img/bg_fallback.jpg
│   └── sfx/click.ogg
└── sd_manifest.json       # tooling-only; not uploaded to the device
```

Minimal `launcher.json`:

```json
{
  "schema_version": 1,
  "id": "my_app",
  "display_name": "我的应用",
  "entry": "scripts/main.lua",
  "args": {},
  "order": 10,
  "visible": true
}
```

`id` must match the deployed directory name and use `[A-Za-z0-9_-]`, 1-63
characters. The current home page does not render `icon` and ignores
`order`/`visible`; do not spend FAT space on an icon.

`sd_manifest.json` (read only by tooling):

```json
{
  "schema_version": 1,
  "app_id": "my_app",
  "assets": [
    {
      "sd": "img/bg.jpg",
      "fallback": "assets/img/bg_fallback.jpg",
      "kind": "jpeg",
      "width": 1024,
      "height": 600,
      "bytes": 180000
    }
  ]
}
```

See [`references/app-package.md`](references/app-package.md) and
[`references/storage-and-fallback.md`](references/storage-and-fallback.md).

## Runtime skeleton

```lua
local lvgl = require("lvgl")
local system = require("system")
local ok_image, image = pcall(require, "image")

local src = debug.getinfo(1, "S").source or ""
if src:sub(1, 1) == "@" then src = src:sub(2) end
local app_dir = src:match("^(.*)/scripts/[^/]+$") or "/fatfs/claw/data/apps/my_app"
local app_id = app_dir:match("([^/]+)$") or "my_app"
local SD = "/sdcard/panel-assets/" .. app_id .. "/"

lvgl.init({ buffer_lines = 40, tick_ms = 5, task_period_ms = 10 })
local font = lvgl.font_load(app_dir .. "/assets/ui_cn.ttf", { size = 20, cache_size = 128 })
if font then lvgl.set_default_font(font) end

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#070a12", pad = 0 })

local function jpeg_image(path, x, y, w, h)
  if not ok_image then return nil end
  local ok, frame = pcall(image.load_file, path)
  if not ok or not frame then return nil end
  local ok2, rgb = pcall(image.convert, frame, image.RGB565)
  pcall(frame.release, frame)
  if not ok2 or not rgb then return nil end
  local ok3, data = pcall(rgb.data, rgb)
  pcall(rgb.release, rgb)
  if not ok3 or not data then return nil end
  local obj = lvgl.image(scr, { x = x, y = y, w = w, h = h, pad = 0 })
  if not pcall(obj.set_raw_rgb565, obj, w, h, data) then
    obj:add_flag("HIDDEN")
    return nil
  end
  return obj
end

local bg = jpeg_image(SD .. "img/bg.jpg", 0, 0, 1024, 600)
local bg_source = "sd"
if not bg then
  bg = jpeg_image(app_dir .. "/assets/img/bg_fallback.jpg", 0, 0, 1024, 600)
  bg_source = bg and "fat" or "programmatic"
end

scr:load()
while not claw.abort_requested() do
  lvgl.process_events(20)
end
lvgl.deinit()
```

For ARGB sprites, read the file with `io.open(path, "rb")`, require exactly
`w*h*4` bytes, then call `set_raw_argb8888(w, h, data)`. For EAF, read the bytes
and pass `src_data` to `lvgl.eaf`; never pass a `/sdcard` path as `src`.

## Validate and deliver

From the skill directory (or with the scripts on `PATH`):

```bash
python scripts/validate_app.py --app-dir <pkg> --remote-id <id> --sd-dir <staging>
python scripts/check_font_coverage.py --ttf <pkg>/assets/ui_cn.ttf --lua <pkg>/scripts/main.lua
python scripts/prepare_sd_assets.py --spec <sd_plan.json>
python scripts/probe_device.py --host http://<device-ip>
python scripts/upload_app.py --host http://<device-ip> --app-dir <pkg> --restart
python scripts/upload_app.py --host http://<device-ip> --app-dir <pkg> --volume sd --restart
python scripts/upload_sd_assets.py --host http://<device-ip> --sd-dir <staging>
```

`upload_app.py` walks the package, creates directories, uploads assets/scripts,
and writes `launcher.json` last. It never uploads `sd_manifest.json`.
`--volume fat` (default) deploys to `/fatfs/claw/data/apps/<id>/` through
`/api/files`; `--volume sd` deploys to `/sdcard/apps/<id>/` through
`/api/sd-files` and skips the FAT budget check (SD upload limit is 64 MiB with
a 256 KiB reserve). Each volume has its own duplicate-id check. The launcher
scans only at boot: pass `--restart` (or use the device Agent's `publish_app`,
which external platforms do not have and which targets FAT). SD asset changes
need only an App relaunch.

If a whole-card endpoint is missing, use the manual route: power off, copy
`sd/panel-assets/<id>/` (assets) or the package to `/sdcard/apps/<id>/` (SD
App), insert the card, boot. The skill works without the endpoints; they only
save a card reader.

## Hard rules and footguns

- `label` has no `text_align`; center with the object's `align="center"`.
- Never call `font:delete()` before `lvgl.deinit()`; let deinit release fonts
  and widgets together. Deleting early causes an instruction-access fault.
- Do not screenshot, read canvas pixels, or probe glyph grids; there is no
  screenshot API and probing can hold the LVGL lock. Validate statically and
  ask the user for a visual check.
- Do not run two UI Apps at once; the display session is exclusive.
- `lvgl.image{src=...}` and `lvgl.eaf{src=...}` cannot read SD; use the raw
  setters / `src_data` instead. `lvgl.font_load("/sdcard/...")` **is** supported
  on this firmware build (extra font root registered at boot).
- `lvgl.create_screen()` only builds a screen — call `scr:load()` after adding
  the widgets and before the event loop, or the App shows a blank background
  while its script runs normally.
- Do not busy-wait; use `system.millis()` and `lvgl.process_events`.
- App Lua can read SD and use `io`/`os`; only install trusted packages.
- The Web API has no authentication. Keep it on a trusted LAN.
- `claw-flash`/factory reset clears runtime Apps on FAT; SD packages and SD
  assets survive.

## Reference map

- [`references/app-package.md`](references/app-package.md) - package/launcher contract.
- [`references/storage-and-fallback.md`](references/storage-and-fallback.md) - FAT/SD tiers, manifest, detection, formats.
- [`references/design-system.md`](references/design-system.md) - grid, tokens, states, anti-template checks.
- [`references/visual-layering.md`](references/visual-layering.md) - L0-L3 art pipeline and memory.
- [`references/lvgl-lua-api.md`](references/lvgl-lua-api.md) - supported APIs and limits.
- [`references/networking.md`](references/networking.md) - net/mqtt/websocket modules, budgets, `next_event` loops, LAN HTTP routes.
- [`references/fonts-cn.md`](references/fonts-cn.md) - Noto subset and coverage.
- [`references/troubleshooting.md`](references/troubleshooting.md) - symptom-to-check table.

Examples live in `assets/examples/`: `countdown` is FAT-only; `sd_fallback` is
a poster-style App with an SD upgrade and same-size FAT fallback. Either package
can be redeployed unchanged with `upload_app.py --volume sd`.

## Evals

`evals/evals.json` contains three real prompts: a Chinese countdown App, a
poster-style SD-backed screen, and a missing-SD/corrupt-asset fallback. Check
the produced package with the scripts above rather than screenshots.
