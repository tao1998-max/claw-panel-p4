# Changelog

## 0.18.0 - 2026-11-03

- New App API — local agent (小爪) from Lua: `claw.ask(prompt[, timeout_ms])`
  (synchronous, default 30 s / max 120 s, blocks the job task, reply is
  auto-spoken when 自动播报 is on), `claw.agent_task(prompt[, name[,
  timeout_ms]])` (background run, returns task_id immediately),
  `claw.agent_task_output(task_id)`, `claw.agent_task_stop(task_id[,
  wait_ms])`. All return `nil, err` when the Agent is off, unconfigured, or
  busy. SKILL.md/README capability lists updated; the device seed (46) ships
  the expanded `claw` module doc.

## 0.17.2 - 2026-11-03

- Firmware baseline sync only; no App API change. Long Claw TTS replies now
  use byte-credit text pacing with the 128 KB PCM buffer, so they play to
  completion without the large-buffer display flash. Claw TTS also pauses
  XiaoZhi's microphone processing while it speaks; `claw.speak` behavior is
  unchanged.

## 0.17.1 - 2026-11-03

- Firmware baseline sync only; no App API change. The board now runs C6
  ESP-Hosted 2.11.7, stores UTC in the RX8025T, starts IM after the first IP,
  uses a 128 KB TTS flow-control buffer, and drives the panel with dual DPI
  framebuffers in Direct mode. App-facing behavior (`claw.speak`, `audio`,
  SD/FAT packages, baseline JPEG icons) is unchanged.

## 0.17.0 - 2026-11-03

- `references/audio.md` gained a **Speech synthesis (TTS)** section and
  `SKILL.md`/README now list `claw.speak(text[, timeout_ms])` as an App
  capability: explicit (not controlled by the Agent auto-speak switch), one
  streaming Doubao session, default 120 s / max 600 s, blocking, emoji
  sanitized, returns `true` or `nil, err`.
- Clarified the split: `claw.speak()` for synthesized speech, `audio` for
  bundled OGG/MP3 effects, music and recording.

## 0.16.1 - 2026-11-02

- `references/lvgl-lua-api.md` documents the firmware's built-in font names:
  `montserrat_14` (default), `20`, `24`, `32` and `48` can be used directly in
  the `font` style (`text_font` is an alias) for ASCII text, with no
  `font_load`. Unknown names are ignored rather than raising an error.

## 0.16.0 - 2026-10-30

- Font lifecycle rules after the serial-terminal work: the TTF subset is a
  static glyph set, so changing Chinese copy without rebuilding it leaves the
  new characters as tofu boxes (a `main.lua`-only upload cannot fix it).
  `references/fonts-cn.md` now documents the rebuild/re-upload flow and the
  memory cost: every `lvgl.font_load()` copies the whole TTF into PSRAM, so
  N sizes = N copies (keep subsets ~100-150 KiB; do not ship full CJK fonts).
- `references/serial.md` gained an "Inside an LVGL App" section: open once via
  `pcall` (the port raises when busy), poll `read()` non-blocking in the UI loop
  (1 KiB RX buffer, 20-40 ms cadence), close on exit, and use blocking reads
  only for request/response steps; a serial/TCP mode switch is described.
- `SKILL.md` footguns and the decision tree now cover the font rebuild rule,
  the per-load PSRAM cost, the display-service exit button overlay at the
  top-right (tapping it stops the App; it is not a reboot), and the UART
  lifecycle.
- `references/troubleshooting.md` gained rows for tofu only on new copy, the
  top-right exit overlay, DTR/RTS resetting the panel when a serial tool
  opens/closes the port, and FAT table damage recovery via `idf.py claw-flash`.
- New eval `serial-terminal-uart` and trigger queries for an in-app UART
  terminal and "copy changed -> boxes" prompts.
- New `assets/examples/anim_lab` example: a 160x160 Lottie loader (absolute SD
  path or package JSON) next to an animimg frame sequence (12 SD frames vs 6
  package fallback frames), plus a pause/resume button. It demonstrates the
  Lottie-vs-LVGL-FS path rules, the no-card fallback, and `validate_skill.py`
  now checks it.
- `references/lvgl-lua-api.md` now documents the rest of the object API:
  `set_pos/get_pos`, `set_size/get_size`, `align`, `on/off` event names
  (`clicked/pressed/released/long_pressed/value_changed/focused/defocused/
  ready/cancel`), flags (`CLICKABLE`/`PRESS_LOCK`/`SCROLLABLE`/`HIDDEN`),
  flex/grid/scroll options, and the type-specific methods (list, table,
  buttonmatrix, calendar, chart, imagebutton, led, menu). The skill scope and
  README feature list mention text input, layout and the generic property API.

## 0.15.0 - 2026-10-30

- Hardware-resource guidance: the device skill index now states that only
  registered resources are claimable. This firmware registers UART1
  (`uart_bus:1`, GPIO26/27) for RS485; general `gpio`/`adc`/`i2c`/... pins are
  not registered and raise resource errors.
- `references/serial.md` warns not to use `gpio` for RS485 DE/RE (direction is
  automatic); trigger queries gained RS485/Modbus/UART1 cases. Device seed
  version bumped to 36 so existing devices refresh the builtin module docs.

## 0.14.0 - 2026-10-30

- RS485/UART guidance: new `references/serial.md` documents UART1
  (TX=GPIO26, RX=GPIO27, 115200 8N1), the official `uart` module, single-owner
  semantics, timeouts, and request/response patterns. Firmware now registers
  `uart_bus:1`, `gpio:26`, `gpio:27` in the ESP-Claw hardware registry and
  releases the port so scripts own it while open.
- `SKILL.md`/`README.md` updated with the serial reference.

## 0.13.0 - 2026-10-30

- LVGL file-system drive `S:` added: `S:/<path>` resolves to `/sdcard/<path>`
  when a card is mounted. LVGL `src` consumers (`lvgl.image{src}`,
  `lvgl.imagebutton`, `lvgl.animimg`, `lvgl.gif`, `lvgl.eaf{src}`) can now read
  SD assets directly; `D:` remains the FAT workspace. Lottie still uses
  absolute `/sdcard/...` because ThorVG bypasses LVGL's FS.
- Docs updated across `SKILL.md`, `references/lvgl-lua-api.md`,
  `references/storage-and-fallback.md`, `references/visual-layering.md` and
  `README.md`; boot-critical assets should still keep a FAT fallback.

## 0.12.0 - 2026-10-30

- Image decoding is enabled on the device: PNG (`LODEPNG`), JPEG (`TJPGD`) and
  GIF, plus an 8 MiB image cache. LVGL file sources (`lvgl.image{src="D:/..."}`,
  `lvgl.animimg`, `lvgl.gif`) decode them from the FAT workspace.
- New Lua widgets: `lvgl.animimg` (PNG/JPEG/`.bin` frame sequences with
  duration/repeat/start/stop) and `lvgl.gif` (loop count, pause/resume/restart,
  frame count, `color_format="rgb565"` for half-size buffers).
- Guidance updated across `SKILL.md`, `references/lvgl-lua-api.md`,
  `references/storage-and-fallback.md` and `references/visual-layering.md`:
  EAF/Lottie remain preferred for long loops; GIF/animimg are CPU-heavy and
  FAT-only.

## 0.11.0 - 2026-10-30

- Lottie animation: `lvgl.lottie` (LVGL + ThorVG) documented across `SKILL.md`,
  `references/lvgl-lua-api.md`, `references/storage-and-fallback.md` and
  `references/visual-layering.md`. `src` may point at `/sdcard` (ThorVG opens
  VFS paths directly), `src_data` also works, playback loops forever, and each
  instance costs `w*h*4` bytes of PSRAM with software rendering.
- Guidance updated: small vector loops can use Lottie instead of converting to
  EAF; keep Lottie regions small and pair it with the same fallback rules.

## 0.10.0 - 2026-10-29

- MP3 HTTP(S) streaming: `audio.player` / `audio.play` accept `http://` and
  `https://` MP3 URLs, including redirects/chunked transfer. Native-rate
  exclusive playback, automatic reconnect (`state="buffering"`, up to 8
  attempts), and `poll().level` (RMS 0..1) for visualizers.
- Added `apps/network_radio` and the matching
  `assets/examples/network_radio/` skill example: neon tuner UI, live
  spectrum, four Chinese stations (AsiaFM/HIT FM/CNR-1), station cards and volume controls; radio
  playback pauses the assistant and restores it on exit.
- `references/audio.md` and the device audio module doc updated; device seed
  version 35.

## 0.9.0 - 2026-10-29

- Native-rate MP3 playback: `audio.player` / `audio.play` accept local `.mp3`
  files and play them at the decoded sample rate/channels (8–48 kHz, stereo,
  16-bit). Decoding runs on a background task; the assistant's AudioService is
  paused and ES8311/I2S is reconfigured during playback, then restored on EOF,
  stop, close or App exit. Wake word/voice and input/recorder/analyzer are
  unavailable while MP3 plays; starting an MP3 while the assistant is busy is
  refused.
- `player:poll()` reports `state="error"` plus an `error` string, native
  `music_info.sample_rate/channels`, and `source_sample_rate/source_channels`.
- `references/audio.md` and the device audio module doc updated; device seed
  version 34.

## 0.8.0 - 2026-10-29

- `image.load_raw(path, w, h[, format])`: loads tightly packed RGB565/GRAY8
  pixels as a frame; pair it with `img:set_frame()` for a zero-copy full-screen
  background with no runtime JPEG decode.
- `scripts/make_raw_background.py`: ffmpeg decode + 8x8 Bayer dither to
  RGB565. Fixes the device JPEG decoder's 4:2:0 chroma blocks in dark
  gradients (visible as purple/red patches in dusk skies).
- `prepare_sd_assets.py` `rgb565` entries accept `"dither": true` for the same
  treatment in the existing SD-asset pipeline.
- Reference guidance updated across SKILL.md, visual-layering,
  storage-and-fallback, troubleshooting, design-system and app-package.
- Raw background is **optional**: the reference weather App stays on JPEG +
  `set_frame` because the raw/dither difference was not visible on the panel.
  Use raw only when dark chroma blocks/banding are actually visible; keep the
  JPEG fallback and the 1.2 MiB raw file on SD.
- Documented the reusable-canvas chart pattern: create one `image.new` buffer,
  redraw with `fill_rect`/`line`, call `set_frame` again; a 920x320 intraday
  chart takes ~50 ms. Added a troubleshooting row for slow per-row
  `string.rep`/`string.sub` builders.
- Reference apps under `apps/` (repo) were migrated to `set_frame` and the
  reusable chart canvas. Device seed version 33.
- Added `references/audio.md`: output/input PCM, OGG/Opus player, WAV recorder,
  level/FFT analyzer, path rules, and the shared-XiaoZhi constraints. Corrected
  the "no image scaling" note: `image.resize` and decode-time scaling exist;
  only LVGL widget zoom is unavailable.

## 0.7.0 - 2026-10-29

- JPEG decode-time scaling: `image.load_file(path, { max_width, max_height })`
  returns an already-decoded RGB565 frame at a 1/2..1/8 decoder stage; best
  effort when the JPEG dimensions cannot be aligned to 8 pixels.
- `image.encode(frame [, opts])` and `image.save_file(path, frame, opts)` accept
  `quality` (1-100) and `subsample` (`"444"`, `"422"`, `"420"`, `"gray"`).
  Measured on a 640x480 canvas: q30/4:2:2 = 10.9 KB, q90/4:4:4 = 26.0 KB.
- `image.crop`, `image.rotate(90|180|270 clockwise)` and `image.flip`
  (PPA hardware, CPU fallback); JPEG rotation can use the decoder's rotation
  stage, avoiding an unrotated intermediate RGB565 buffer.
- `image.resize` on an undecoded JPEG now decodes at a reduced stage and then
  applies PPA scaling; RGB565 resize after conversion also prefers PPA
  (640x480 -> 160x120 in ~46 ms).
- Opaque canvas fills now paint one row and `memcpy` across the rectangle:
  a full 1024x600 fill is ~19 ms instead of ~64 ms (~3.4x). Use opaque fills
  for backgrounds.
- Device seed version 31 ships the updated image module doc.

## 0.6.0 - 2026-10-29

- `image.new(width, height, image.RGB565|GRAY8)` canvas plus `fill`,
  `fill_rect`, `line` and `blit` (blit converts JPEG/any frame, clips, blends).
- `img:set_frame(frame)` zero-copy display handoff in `lvgl`: converts to
  RGB565 once and keeps the image store alive (measured 0.16 ms/call vs two
  1.2 MB copies for `frame:data()` + `set_raw_rgb565`).
- Converted formats are cached per frame; documenting convert-once patterns.
- Design-system guidance: data-dense screens are drawn on canvases, not with
  per-pixel Lua strings.

## 0.5.0 - 2026-10-29

- Home page is now a six-column phone-style grid: each tile shows the App icon,
  `display_name` and `version`; missing icons fall back to a gradient tile with
  the first character.
- `launcher.json` gained optional `version` (1-31 chars, semver recommended)
  and renders `icon` (`assets/icon.jpg`, 96x96 baseline JPEG). `validate_app.py`
  checks the file, dimensions and JPEG type (progressive is rejected) and warns
  on missing version/icon.
- `make_app_icons.py` generates 96x96 baseline JPEG icons from simple shapes
  (ffmpeg, no Pillow); `/api/apps` and `run_app.py --list` show versions, and
  `upload_app.py` prints the version transition (old -> new).

## 0.4.0 - 2026-10-29

- Device debug endpoint `GET /api/lua-failures?limit=4`: recent failed /
  timed-out Lua jobs with `id/name/path/status/error` (also logged on serial;
  stopped jobs are excluded). `probe_device.py` prints it as `Lua failures: ...`.
- `check_lua_pitfalls.py`: dependency-free static checker for the pitfalls that
  make an App exit immediately — Lua keywords as method names (`u:local()`),
  `:on("click")`, float coordinates in LVGL options, and `and/or` truncating
  multiple return values.
- App launch / self-test API: `GET /api/apps`, `POST /api/apps/refresh`,
  `POST /api/apps/<id>/launch`, `POST /api/apps/<id>/test?duration_ms=N`
  (launch, run, stop, report `running|finished|failed` + `path:line: reason`),
  `GET /api/lua-tasks`, `POST /api/lua-tasks/<id>/stop`.
- `run_app.py` wraps the launch/test endpoints (with `--refresh`, `--list`,
  `--launch`), so an external AI can verify an App without a human tap.
- `upload_app.py` now calls `POST /api/apps/refresh` after uploading, so
  deploying an App no longer reboots the device (`--restart` and `--no-refresh`
  remain available).
- Stopped jobs are excluded from the failure ring; only real failures and
  timeouts are reported.
- SKILL.md/troubleshooting document the headless verify loop.

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
