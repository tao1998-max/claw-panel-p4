# App Package Contract

## Root

Runtime Apps live in the writable FAT workspace, and optionally on the SD card:

```text
/fatfs/claw/data/apps/<app_id>/   # scanned first, writable, wins on duplicate id
/sdcard/apps/<app_id>/            # optional, read-only, launched in place
```

Both roots use the identical layout. SD packages require the card to be inserted
before boot (no hot-plug) and a restart after any change; with no card, only FAT
apps are listed. `publish_app`/`remove_app` still target the FAT root — manage
SD packages through the Web file manager (`/api/sd-files`). An SD package may
keep its TTF inside the package and load it by absolute path
(`/sdcard/apps/<app_id>/assets/<font>.ttf`); the FAT form
`D:apps/<app_id>/assets/<font>.ttf` also works on both roots.

## Layout

```text
<app_id>/
├── launcher.json
├── scripts/
│   └── main.lua
├── assets/
│   ├── ui_cn.ttf
│   ├── img/bg_fallback.jpg
│   └── sfx/click.ogg
└── sd_manifest.json       # tooling-only
```

Write `launcher.json` last when uploading so the launcher never sees a
half-written package.

## launcher.json

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

- `schema_version` must be integer `1`.
- `id` is required, 1-63 ASCII `[A-Za-z0-9_-]`, and must equal the directory
  name. The deployed directory name is authoritative.
- `display_name` defaults to `id`.
- `entry` is required, package-relative, ends in `.lua`, exists, and is at most
  64 KiB (the Lua runtime's script limit).
- `icon` is optional `.jpg`/`.jpeg`; the current home page does not render it,
  so omit it to save FAT space.
- `args` is an object; `order` is an integer; `visible` is a boolean. The
  current home page ignores `order` and `visible`.
- Paths must be package-relative: no leading `/`, no `..`, no backslashes.
- The encoded manifest must be <=4096 bytes.
- Unknown fields are ignored; preserve fields you are not changing.

## Package budget

The reference device has a 4 MiB FAT shared with AI assets and the Claw
workspace; only about 1.4 MiB was free after the three example Apps were
installed. Budget per App:

- target: <=300 KiB;
- warning: >512 KiB;
- hard limit: >1 MiB only with an explicit override.

FAT allocation is 4 KiB, so a 153-byte manifest still consumes one cluster.
Heavy images, EAF, and extra audio belong on SD.

## Upload and refresh

- Web API base: `/fatfs/claw`; upload a package path as
  `/data/apps/<id>/...`.
- Create parent directories first, upload scripts/assets, then
  `launcher.json`.
- HTTP uploads do **not** reload the app registry. Restart the device
  (`POST /api/restart`) before the launcher shows a new/updated App.
- The device-side Agent's `publish_app` is not available to external AI
  platforms.
- `claw-flash` or a factory reset clears runtime Apps on FAT; keep the source
  package on the host.
