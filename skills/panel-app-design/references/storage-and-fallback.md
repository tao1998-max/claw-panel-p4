# Storage Tiers and SD Fallback

## Verified device state (reference build)

Boot log on the reference panel:

```text
hw.sd: sdcard mounted at /sdcard (480 MB, 8 files)
claw_storage: workspace mounted at /fatfs: 4020 KB total, 1404 KB free
```

The SD card is a separate FAT volume mounted once at boot. It has no card-detect
line and no hot-plug. `format_if_mount_failed` is off, and only FAT (not exFAT)
is supported. Its `max_files` is 5.

The launcher scans two App roots: `/fatfs/claw/data/apps` (writable, scanned
first) and `/sdcard/apps` (read-only, apps are launched in place from the
card). A package on SD has the same layout as on FAT; when both roots hold the
same `id`, the FAT copy wins and the SD copy is ignored. With no card inserted
before boot the SD root does not exist, so only FAT apps are listed (and the
home page is empty if FAT has none). `/api/status` does not expose storage;
`/api/storage` does on firmware that includes it:

```json
{
  "fat": {"mount": "/fatfs", "mounted": true,
          "total_bytes": 4116480, "free_bytes": 1241088},
  "sd":  {"mount": "/sdcard", "mounted": true,
          "total_bytes": 503152640, "free_bytes": 126730240},
  "sd_assets": {"root": "/sdcard/panel-assets", "exists": true}
}
```

The endpoint also includes `total`, `free`, `total_kb`, and `free_kb` aliases.

## Tier table

| Tier | Path | Use | Required |
|---|---|---|---|
| FAT package | `/fatfs/claw/data/apps/<id>/` | launcher, scripts, TTF subset, same-size low-quality fallbacks, tiny SFX | yes (unless the package lives on SD) |
| SD package | `/sdcard/apps/<id>/` | full package: `launcher.json`, scripts, images, SFX; launched in place, read-only | no |
| SD library | `/sdcard/panel-assets/<id>/` | high-quality JPEG, raw RGB565/ARGB8888, EAF, extra OGG | no |
| Never SD | — | `lvgl.image{src}`, `lvgl.eaf{src}` (use `image.load_file()` / EAF `src_data` instead) | — |

`lvgl.font_load()` also accepts absolute `/sdcard/...` paths in this build, so
an SD package can ship its own TTF. On older firmware keep it on FAT and load
via `D:apps/<id>/...`.

## Runtime resolution

```lua
local SD = "/sdcard/panel-assets/" .. app_id .. "/"

local function jpeg(path, x, y, w, h) ... end

local bg = jpeg(SD .. "img/bg.jpg", 0, 0, 1024, 600)
local source = "sd"
if not bg then
  bg = jpeg(app_dir .. "/assets/img/bg_fallback.jpg", 0, 0, 1024, 600)
  source = bg and "fat" or "programmatic"
end
```

Use `pcall` around every file load. A missing card, missing file, or corrupt
JPEG is normal; fall back silently and log one short line. Do not retry mounting
or block the UI.

## Supported per-tier formats

| Asset | SD | FAT fallback |
|---|---|---|
| full-screen scene | baseline JPEG, yuv420, <=4 MiB, <=1920x1080 | same 1024x600 dimensions, lower JPEG quality |
| opaque rectangle | JPEG | same-dimension JPEG |
| transparent sprite | raw ARGB8888 (`B,G,R,A`, `w*h*4`) | same-dimension opaque JPEG or programmatic panel |
| animation | EAF, passed as `src_data` | optional package EAF or skip |
| audio | OGG, absolute `audio.play("/sdcard/...")` | OGG under `assets/sfx/` |
| font | not supported | TTF subset under `assets/`, `lvgl.font_load` |
| app code | not supported | `scripts/*.lua` |

> The "SD" column above is the optional **library** tier
> (`/sdcard/panel-assets/<id>/`). A full **SD App package**
> (`/sdcard/apps/<id>/`) is different: it is launched in place and may carry its
> own `scripts/`, TTF and images. `lvgl.font_load("/sdcard/...")` is supported on
> this build (extra font root registered at boot); see `SKILL.md` and
> `app-package.md`.

Source PNG/WebP/GIF is an **authoring** format only. `image.load_file()` reads
baseline JPEG; LVGL has no PNG/GIF decoder on this build. Convert offline.

There is no runtime scaling API. `set_raw_rgb565`/`set_raw_argb8888` draw at the
source dimensions; a 512x300 fallback will not fill a 1024x600 screen. Use the
same dimensions and lower quality/contrast instead.

## sd_manifest.json

Tooling-only; the App does not parse it. Keep it under 4 KiB.

```json
{
  "schema_version": 1,
  "app_id": "sd_fallback",
  "assets": [
    {
      "sd": "img/bg.jpg",
      "fallback": "assets/img/bg_fallback.jpg",
      "kind": "jpeg",
      "width": 1024,
      "height": 600,
      "bytes": 19486
    },
    {
      "sd": "img/sigil.argb",
      "fallback": "assets/img/sigil_fallback.jpg",
      "kind": "argb8888",
      "width": 180,
      "height": 180,
      "bytes": 129600
    }
  ]
}
```

- `sd` is relative to `/sdcard/panel-assets/<id>/`.
- `fallback` is relative to the package.
- `kind` is `jpeg`, `rgb565`, `argb8888`, `eaf`, or `ogg`.
- First-screen assets need a fallback; decorative EAF/audio may be marked
  `"optional": true` with no fallback, but the App must skip them cleanly.
- Never map `.lua` or `.ttf` to SD.

## Memory and file limits

- full-screen RGB565 1024x600 = 1.23 MiB; ARGB8888 = 2.46 MiB;
- keep one full-screen bitmap alive; release `image.frame` and converted views;
- SD `max_files=5` is shared by `io`, image/audio, HTTP upload/download;
- SD cluster size is 64 KiB; avoid thousands of tiny files;
- `storage.read_file`/`io` can read large SD files, but the Lua string and the
  LVGL copy coexist; keep EAF modest;
- `storage.get_free_space()` reports FAT only. Use `/api/storage` on the host
  for SD free space, or host-side `esp_vfs_fat_info`.

## Delivery

### FAT package

`upload_app.py` validates the package, preflights `/api/storage`, creates
directories, uploads assets/scripts, then `launcher.json`. Pass `--restart` for
the launcher to see it.

### SD assets

Preferred when firmware exposes the endpoint:

```bash
python scripts/upload_sd_assets.py --host http://<ip> --sd-dir <staging>
```

The endpoint accepts paths relative to `/sdcard/panel-assets`; it refuses `..`,
backslashes, control characters, and root deletion. It returns 503 when no card
is mounted and 507 when free space is insufficient.

Manual route (always works): power off, copy `panel-assets/<id>/` to the card,
insert, boot. SD changes need only an App relaunch, not a device restart.

Security: there is no HTTP authentication. `/api/sd-assets` is restricted to
`/sdcard/panel-assets`, but the Web server must stay on a trusted LAN.
