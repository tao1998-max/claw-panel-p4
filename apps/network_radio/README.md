# 网络电台 / NEON RADIO

A neon cyberpunk tuner for the 1024x600 panel. It streams Chinese MP3 radio
at the source's native rate (44.1 kHz stereo) and draws a live spectrum from
the MP3 decoder's RMS level. The UI is in Chinese.

## Controls

- Tap a station card to switch and start it.
- `播放` / `停止` toggles the current station.
- `-` / `+` changes 音量 in 10% steps.
- The top-right clock uses the system time.

## Stations

Defined in the `STATIONS` table at the top of `scripts/main.lua`:

| 电台 | URL |
|---|---|
| 亚洲音乐台 | `https://lhttp.qtfm.cn/live/15318341/64k.mp3` |
| 轻松调频 | `https://lhttp.qtfm.cn/live/15318703/64k.mp3` |
| 中国之声 | `https://lhttp.qtfm.cn/live/15318317/64k.mp3` |
| 亚洲热歌 | `http://hot.asiafm.net:8000/asiafm` |

Only MP3 streams are supported (no AAC/HLS). HTTP and HTTPS both work.

## Audio behaviour

Radio uses the native-rate MP3 exclusive path: the assistant's AudioService is
paused while radio plays, then the codec returns to 16 kHz and the assistant
restarts on stop or App exit. While radio plays, wake word/voice,
recording and the analyzer are unavailable. A dropped stream reconnects
automatically and the UI shows `TUNING`.

## Deploy

```bash
python skills/panel-app-design/scripts/upload_app.py --host <device-ip> \
  --app-dir apps/network_radio --volume sd --force
```
