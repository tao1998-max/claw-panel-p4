# Troubleshooting

| Symptom | Check | Fix |
|---|---|---|
| Only the background/system screen appears | Did the App call `scr:load()`? Is the entry the one being launched? | Call `scr:load()` after creating objects; re-check `launcher.json` |
| Chinese shows tofu boxes | TTF path, `font_load` return, coverage | Put the TTF in `assets/`, rerun `check_font_coverage.py` |
| Text is left-aligned despite centering code | `text_align` is not implemented | Use the object's `align="center"` and remove fixed widths that fight it |
| Buttons overlap | Row math (`y`, `h`, gaps) | Recompute the wireframe; leave 8-20 px between pressable rows |
| App exits with `Instruction access fault` | `font:delete()` before `lvgl.deinit()`? | Remove manual font deletion; let deinit clean up |
| App freezes / `lvgl lock failed` | A second UI App or a blocking loop | Run one UI App; never busy-wait; check the session lock owner |
| Image is blank | Used `lvgl.image{src="...jpg"}` or `/sdcard` | Use `image.load_file` + `set_raw_rgb565`; check SD mount |
| JPEG fails to load | 4:4:4, progressive, >4 MiB, >1920x1080 | Re-encode baseline 4:2:0 within limits |
| SD image does not fill the screen | Fallback/source dimensions differ | Regenerate both at 1024x600; there is no runtime scale |
| SD asset never loads | Card inserted after boot; wrong `app_id` path | Insert before power-on; verify `/sdcard/panel-assets/<id>/` |
| SD API returns 503 | Card not mounted | Insert card, reboot; mount happens once at boot |
| SD upload returns 507 | Free space below file + 256 KiB reserve | Free space or upload smaller assets |
| SD upload returns 400 parent_not_found | Parent dir not created | Call `/api/sd-assets/mkdir` first |
| Web upload returns 400 | Absolute path, `..`, or missing parent | Use `/data/apps/<id>/...`; create parents first |
| App uploads but home page does not show it | Registry scans only at boot | `POST /api/restart` (or pass `upload_app.py --restart`) |
| SD App missing from home (FAT Apps still show) | Card inserted before boot? Path is exactly `/sdcard/apps/<id>/` with `launcher.json` `id` == dir name? | Fix the layout and restart; confirm the card is mounted (`probe_device.py`) |
| SD App is ignored even though it is on the card | Same `id` already exists on FAT (FAT is scanned first and shadows it) | Delete the FAT copy, or give the SD copy a different id |
| SD App Chinese shows tofu boxes | Font path not an absolute `/sdcard/...` under the mount, or the TTF was not uploaded | Load by absolute path; verify the file with `GET /api/sd-files?path=/apps/<id>/assets` |
| SD App upload returns 400 parent_not_found | `/api/sd-files/upload` needs an existing parent | `POST /api/sd-files/mkdir {"path":"/apps/<id>/...","recursive":true}` first |
| App missing after reflash | `claw-flash` clears runtime Apps on FAT | Re-upload FAT packages; SD packages and SD assets survive |
| EAF does not animate | `src=` used with SD, or GIF passed | Read bytes and pass `src_data`; convert GIF to EAF offline |
| Full-screen animation stutters | Re-uploading a 1.23 MiB frame every tick | Animate a small region/sprite; reduce frame rate |
| App can't read a config file | Used `claw.file`/`cap_files`, which are sandboxed | Use `io`/`storage` with an allowed absolute path |
| Device unreachable | IP changed or LAN isolation | `probe_device.py --host <ip>`; check Wi-Fi status |
| `net`/`mqtt`/`websocket` is nil or `module not found` | Firmware predates the network modules | `GET /api/lua-modules`; update the firmware (seed 28+) |
| `socket budget exhausted` | Too many simultaneous handles in one job (3 sockets, 1 TLS) | Close handles when done; keep one MQTT/WS client per script |
| MQTT connects but never receives | Broker did not deliver the client's own publish (ACL or wrong topic) | Subscribe and publish on the same topic; check ACL/case; verify with mosquitto |
| WebSocket connect timeout | Wrong URL/port, firewall, or `ws://` vs `wss://` mismatch | Try both schemes; check the LAN firewall; test the echo server from a PC |
| `net.http` "connect failed" on HTTPS | TLS needs the CA bundle or `ca_pem`; `insecure` needs firmware support | Drop `insecure`, supply `ca_pem`, or check `/api/lua-modules` |

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
