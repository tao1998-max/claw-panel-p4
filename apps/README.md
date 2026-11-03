# Panel Apps

Source copies of the live 1024x600 panel apps, matching what is installed on
the device. Deploy or update one from the project root:

```bash
python skills/panel-app-design/scripts/upload_app.py --host <device-ip> \
  --app-dir apps/<id> --volume sd --force
```

- `answer_book`, `blackjack`, `countdown`, `morse_station` also run from FAT.
- `ashare_watch`, `weather_panel`, `mqtt_panel`, `network_radio` are SD packages.
- `network_radio` streams MP3 radio over HTTP(S) at native rate. It pauses the
  assistant audio while playing; wake word/voice returns when the App exits or
  playback stops. Edit the `STATIONS` table in its `main.lua` to change streams.
- Backgrounds use `img:set_frame()` (decode once, zero-copy). `ashare_watch`
  draws its intraday chart into a reusable RGB565 canvas with `image.new()` +
  `fill_rect`; the old per-pixel Lua string path is gone.
- `weather_panel` keeps a 1024x600 JPEG fallback and no longer ships the 1.2 MiB
  raw background (`bg.rgb565`).
