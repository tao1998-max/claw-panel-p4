# Troubleshooting

| Symptom | Check | Fix |
|---|---|---|
| Only the background/system screen appears | Did the App call `scr:load()`? Is the entry the one being launched? | Call `scr:load()` after creating objects; re-check `launcher.json` |
| Chinese shows tofu boxes | TTF path, `font_load` return, coverage | Put the TTF in `assets/`, rerun `check_font_coverage.py` |
| Boxes appear only on newly added Chinese copy | The subset is a static glyph set; only `main.lua` was uploaded | Rerun `make_cjk_subset.py` for the final copy, re-upload the TTF, relaunch |
| App closes by itself when the top-right corner is touched | `display_service` overlays a 64x40 exit button there; log says `exit gesture requested Lua job stop` | Move counters/buttons/text away from the top-right corner; this is an exit, not a crash or reboot |
| Panel reboots when a serial terminal opens or closes the port | USB-serial DTR/RTS are wired to EN/IO0 | Keep DTR/RTS low in the capture tool; the App is not involved |
| FAT reports 0 free and files larger than 4 KiB fail to read | FAT table damage after an unclean shutdown | Back up readable files, then `idf.py claw-flash` (rewrites the 4 MiB image; runtime FAT Apps are lost, SD packages survive) |
| Text is left-aligned despite centering code | `text_align` is not implemented | Use the object's `align="center"` and remove fixed widths that fight it |
| Buttons overlap | Row math (`y`, `h`, gaps) | Recompute the wireframe; leave 8-20 px between pressable rows |
| App exits with `Instruction access fault` | `font:delete()` before `lvgl.deinit()`? | Remove manual font deletion; let deinit clean up |
| App freezes / `lvgl lock failed` | A second UI App or a blocking loop | Run one UI App; never busy-wait; check the session lock owner |
| Image is blank | LVGL `src` path is not `D:/...`/`S:/...`, the card is missing, or the format is unsupported | Use `D:/`/`S:/` for LVGL sources, or `image.load_file` + `img:set_frame(frame)`; check the mount |
| JPEG fails to load | progressive, >4 MiB, >1920x1080, or 4:4:4/4:2:2 from an encoder using non-standard sampling (ffmpeg's variants were rejected) | Use baseline 4:2:0, or a 4:4:4 file produced by the device's `image.encode(..., {subsample="444"})` |
| SD image does not fill the screen | Fallback/source dimensions differ | Regenerate both at 1024x600; runtime `image.resize()` works but costs time/memory |
| Thumbnail uses too much memory | Full-size JPEG decoded for a small tile | `image.load_file(path, {max_width=..., max_height=...})` decodes at 1/2..1/8 |
| Chart redraw stutters / App feels slow | Rows built with `string.rep`/`string.sub`/`table.concat`, or `frame:data()` copies | Reuse one `image.new` canvas, draw with `fill_rect`/`line`, then `img:set_frame(canvas)` |
| MP3 play returns `nil, "assistant busy..."` | Assistant connecting/listening/speaking; MP3 needs the exclusive codec | Retry when idle; MP3 pauses the assistant, so wake word/voice is unavailable while it plays |
| MP3 has no sound but `play` returned true | Check `player:poll().state/error`; assistant may have been busy or the codec exclusive open failed | Wait for `finished`/`stopped`; retry if `state=="error"` (`audio.md` MP3 section) |
| Dark background shows coloured blocks / banding | Large JPEG decoded on device; 4:2:0 chroma upsampling + RGB565 quantization | Generate a dithered raw with `make_raw_background.py`, load via `image.load_raw` + `set_frame`; keep the JPEG as fallback |
| SD asset never loads | Card inserted after boot; wrong `app_id` path | Insert before power-on; verify `/sdcard/panel-assets/<id>/` |
| SD API returns 503 | Card not mounted | Insert card, reboot; mount happens once at boot |
| SD upload returns 507 | Free space below file + 256 KiB reserve | Free space or upload smaller assets |
| SD upload returns 400 parent_not_found | Parent dir not created | Call `/api/sd-assets/mkdir` first |
| Web upload returns 400 | Absolute path, `..`, or missing parent | Use `/data/apps/<id>/...`; create parents first |
| App uploads but home page does not show it | Registry scans at boot; a Web-file-manager upload does not refresh it | `POST /api/apps/refresh`, or use `upload_app.py` (refreshes automatically) |
| SD App missing from home (FAT Apps still show) | Card inserted before boot? Path is exactly `/sdcard/apps/<id>/` with `launcher.json` `id` == dir name? | Fix the layout and restart; confirm the card is mounted (`probe_device.py`) |
| SD App is ignored even though it is on the card | Same `id` already exists on FAT (FAT is scanned first and shadows it) | Delete the FAT copy, or give the SD copy a different id |
| SD App Chinese shows tofu boxes | Font path not an absolute `/sdcard/...` under the mount, or the TTF was not uploaded | Load by absolute path; verify the file with `GET /api/sd-files?path=/apps/<id>/assets` |
| SD App upload returns 400 parent_not_found | `/api/sd-files/upload` needs an existing parent | `POST /api/sd-files/mkdir {"path":"/apps/<id>/...","recursive":true}` first |
| App missing after reflash | `claw-flash` clears runtime Apps on FAT | Re-upload FAT packages; SD packages and SD assets survive |
| EAF does not animate | `src=` path is not `D:/`/`S:/`, the file is missing, or a GIF was passed as EAF | Use an LVGL FS path (`D:/...` or `S:/...`) or `src_data` bytes; GIF needs `lvgl.gif` or offline conversion to EAF |
| Full-screen animation stutters | Re-uploading a 1.23 MiB frame every tick | Animate a small region/sprite; reduce frame rate |
| App can't read a config file | Used `claw.file`/`cap_files`, which are sandboxed | Use `io`/`storage` with an allowed absolute path |
| Device unreachable | IP changed or LAN isolation | `probe_device.py --host <ip>`; check Wi-Fi status |
| `net`/`mqtt`/`websocket` is nil or `module not found` | Firmware predates the network modules | `GET /api/lua-modules`; update the firmware (seed 28+) |
| `socket budget exhausted` | Too many simultaneous handles in one job (3 sockets, 1 TLS) | Close handles when done; keep one MQTT/WS client per script |
| MQTT connects but never receives | Broker did not deliver the client's own publish (ACL or wrong topic) | Subscribe and publish on the same topic; check ACL/case; verify with mosquitto |
| WebSocket connect timeout | Wrong URL/port, firewall, or `ws://` vs `wss://` mismatch | Try both schemes; check the LAN firewall; test the echo server from a PC |
| `net.http` "connect failed" on HTTPS | TLS needs the CA bundle or `ca_pem`; `insecure` needs firmware support | Drop `insecure`, supply `ca_pem`, or check `/api/lua-modules` |
| App exits immediately after launch | A Lua error before the first frame | `run_app.py --app-id <id> --duration 3 --refresh` (or `POST /api/apps/<id>/test`) returns `path:line: reason`; fix and re-upload |
| Home tile has no icon (gradient with a letter instead) | Missing icon, wrong path, progressive/oversized JPEG, or unreachable file | Ship `assets/icon.jpg` (96x96 baseline) and set `"icon"`; regenerate with `make_app_icons.py` |
| Home tile shows no version | `launcher.json` has no `version` | Add `"version": "1.0.0"` and refresh the registry |
| App name is cut with "..." on the tile | `display_name` is too long for one line | Keep the name short; the tile shows about 8 CJK characters |
| Crash right after a fetch completes | `cond and f() or g()` truncates a multi-value `f`/`g` to one value (`attempt to index a nil value` at the caller) | Use explicit `if/else` returns; add `value = value or fallback` for optional returns |
| `lvgl option 'x'/'y' must be an integer` | Lua 5.4+ `/` always returns a float (`320 / 2` = `160.0`) and LVGL options reject floats | Use `//` (`CH_H // 2`) or `math.floor(...)` for every coordinate/width/height |

## Fallback verification

1. Install the App with no `/sdcard/panel-assets/<id>/`; launch. It must render
   the package fallback and log `bg=fat`.
2. Upload the SD tree; relaunch. It should log `bg=sd`.
3. Delete one SD asset; relaunch. That layer should fall back without a crash.
4. Power off, remove the card, boot, launch. A FAT package must still start
   (fallback only); an SD-hosted package is expected to be absent from the home
   page instead of starting.

If step 1 shows a blank screen, the package fallback is missing, has the wrong
dimensions, or the JPEG decode failed. Check the serial/App log first; there is
no screenshot API.
