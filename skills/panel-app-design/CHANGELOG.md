# Changelog

## 0.3.0 - 2026-10-29

- New `references/networking.md`: `net` (TCP/UDP/DNS/HTTP), `mqtt` and
  `websocket` Lua modules, `next_event()` event loops, socket budgets, and the
  now-enabled Lua `http_server` LAN routes.
- Example `assets/examples/mqtt_panel/`: MQTT heartbeat + `color:` / `text:` /
  `ping` commands, reconnect status on screen.
- `upload_app.py --volume sd` deploys to `/sdcard/apps/<id>`; `probe_device.py`
  lists SD-hosted apps.
- Skill docs gained SD-package and SD-font notes; device seed version 28 now
  ships the three module docs.

## 0.2.0 - 2026-10-29

- App packages may live on the SD card (`/sdcard/apps/<id>/`) and are launched
  in place; FAT is scanned first and shadows a duplicate id. No card at boot
  means SD apps are absent, not errors.
- `lvgl.font_load` accepts absolute `/sdcard/...` paths (SD mount registered as
  an extra font root), so an SD package can bundle its own TTF. `D:apps/...`
  still resolves against FAT.
- New whole-card Web endpoints: `/api/sd-files` (list/delete/mkdir/upload/file)
  and `/sd-files/*` direct downloads; `/api/sd-assets` stays restricted to
  `panel-assets/`.
- `upload_app.py --volume {fat,sd}`; SD deploys skip the FAT budget and keep a
  256 KiB reserve. Device Claw page file manager gained an internal/SD toggle.
- Troubleshooting: blank screen despite a running script -> missing
  `scr:load()`; SD app shadowed by a duplicate FAT id; SD font path mistakes.

## 0.1.0 - 2026-10-28

- Initial external-AI skill for 1024x600 ESP32-P4 launcher Apps.
- FAT package contract, Chinese TTF subset workflow, poster L0-L3 layering.
- SD runtime asset fallback contract with same-size FAT fallbacks and
  `sd_manifest.json`.
- Scripts: validate skill/app, font subset/coverage, SD asset preparation,
  FAT/SD upload, device storage probe.
- Examples: FAT-only countdown, SD-backed poster demo.
- Firmware support: `GET /api/storage`, restricted `/api/sd-assets`.
