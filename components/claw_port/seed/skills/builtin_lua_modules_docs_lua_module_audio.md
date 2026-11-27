# audio

Audio output, microphone input, recording, analysis, OGG/Opus effects, and
native-rate MP3 playback (local files or HTTP(S) radio streams) through the
on-board ES8311. Load it with `require("audio")`.

The module uses the XiaoZhi `AudioService` that owns the codec/I2S; it never
opens a second codec. While XiaoZhi is connecting, listening, or speaking,
output is rejected so game audio never talks over the assistant.
Recording/analysis reads the same 10 ms frames that feed WakeNet/AFE, so it
does not create a second microphone reader.

MP3 is the exception: to play at the file's native rate, the assistant's
AudioService is paused, ES8311/I2S are reconfigured, and the service is
restarted when playback ends. See "MP3 (native rate)" below.

## Output

```lua
local out = audio.open_output({ volume = 60 })
local info = out:info() -- { sample_rate = 16000, channels = 1, bits = 16 }
out:set_volume(70)
out:write(pcm)          -- 16 kHz mono 16-bit, byte-aligned string
out:play_tone(880, 200) -- frequency Hz, duration ms (max 5000)
out:close()
```

Methods: `info`, `set_volume(0..100)`, `get_volume`, `set_mute(bool)` (no-op
on the shared output), `write(pcm)`, `play_tone(freq_hz, duration_ms)`,
`close`. Only one output may be open. This PCM/tone path stays 16 kHz mono;
MP3 playback bypasses it at the file's native rate.

## Input

```lua
local input = audio.open_input({})
local pcm = input:read(3200) -- 1600 samples = 100 ms, nil on timeout
input:close()
```

Only 16000 Hz mono 16-bit is supported in this version. `read(bytes)` may
also take `{ bytes = n }`. Input is unavailable when XiaoZhi is disabled or
the microphone is muted.

## Player

```lua
local player = audio.player({ output = out })
player:play("sfx/win", { wait = false })
local state = player:poll()
-- { state = "buffering"|"playing"|"finished"|"stopped"|"idle"|"error",
--   running, level = 0..1, error?,
--   music_info = { sample_rate, channels, bits, bitrate,
--                  source_sample_rate, source_channels } }
player:stop()
player:close()
```

- Local OGG/Opus files go through the assistant's 16 kHz path. The demuxer
  only understands OGG containers holding an **Opus** stream: OGG-Vorbis
  files are dropped silently (no error, no log), so always encode sfx with
  `ffmpeg -c:a libopus -b:a 24k -ar 16000 -ac 1`. Verify with
  `head -c 40 file.ogg | grep -a OpusHead`.
- Local `.mp3` files and `http://` / `https://` MP3 streams play at their
  **native** sample rate and channel count.
- `wait = true` waits for the file to finish (60 s cap); a live stream does
  not finish, so use `poll`/`stop` for radio.
- `pause`/`resume` return `nil, "unsupported"`; AAC/HLS playback is not
  implemented and there is no seek.

## MP3 and radio streams (native rate)

```lua
local player = audio.player({ output = out })
player:play("/sdcard/music/song.mp3", { wait = false })
player:play("http://ice1.somafm.com/groovesalad-128-mp3", { wait = false })
local ev = player:poll()
-- ev.music_info.sample_rate == 44100, channels == 2 for a 44.1k stereo stream
-- ev.level is the RMS (0..1) of the last decoded frame, for visualizers
player:stop()
```

- MP3 URLs support HTTP and HTTPS, redirects and chunked transfer. ICY
  metadata is not requested; the body must be an MP3 stream.
- A dropped stream reconnects automatically (up to 8 tries) and reports
  `state = "buffering"`; use `stop()` to end.
- Streams play at their native rate; a mid-stream format change or an AAC/HLS
  stream is rejected.

- MP3 starts only when the assistant is idle. If it is connecting, listening
  or speaking, `play()` returns `nil, err`; retry after the exchange.
- During MP3 playback the assistant's AudioService is paused and the shared
  ES8311/I2S is reconfigured to the MP3 rate (8–48 kHz). **Wake word and voice
  are unavailable while the MP3 plays.** On EOF, `stop()`, `close()` or App
  exit, the codec is restored to 16 kHz and the assistant restarts.
- Mono MP3 is duplicated to stereo; 16-bit PCM only. A sample-rate or channel
  change inside one file is rejected.
- One MP3 plays at a time; starting another stops the first.
- `music_info.sample_rate/channels` report the actual output format; the
  `source_*` fields report what the MP3 decoder produced.
- `state == "error"` carries an `error` string (`assistant busy`, `file
  missing`, `MP3 decoder unavailable`, ...).
- Use `audio.player` if the App needs `stop()`/`poll()`; the legacy
  `audio.play("x.mp3")` is fire-and-forget and only stops on App exit or EOF.
- Recording/analysis (`open_input`) is unavailable while MP3 is playing
  because the AudioService input path is paused.

## Recorder

```lua
local recorder = audio.recorder({ input = input })
local meta = recorder:record("test.wav", { duration_ms = 1000 })
-- meta = { path=..., duration_ms=..., bytes=..., format="wav" }
recorder:close()
```

Writes 16 kHz mono 16-bit WAV. Relative names resolve under the app root;
bare names go to `<app>/recordings/<name>.wav`. Writable roots are
`/fatfs/claw/data/` and `/sdcard/`. Keep the duration within free FAT space.

## Analyzer

```lua
local analyzer = audio.analyzer({ input = input })
local level = analyzer:read_level(100) -- { rms, peak, duration_ms }
local spectrum = analyzer:read_spectrum(512, 16)
-- { bands = {0..255, ...}, peak_freq_hz, peak_db, rms,
--   fft_size, band_count, sample_rate }
analyzer:close()
```

`fft_size` must be a power of two from 64 to 4096; `bands` is 1..fft_size/2.
The spectrum uses a Hann window and maps -90..-20 dB to 0..255.

## Legacy effect playback

```lua
audio.play("deal")        -- <app>/assets/sfx/deal.ogg
audio.play("sfx/win")     -- <app>/assets/sfx/win.ogg
audio.play("music/song.mp3") -- native-rate MP3, fire-and-forget
audio.play("/sdcard/effects/push.ogg")
audio.is_available()
```

App-relative paths are resolved from the running script's app root. Allowed
absolute roots are `/fatfs/claw/data/`, `/fatfs/claw/system/`, `/fatfs/ai/`,
and `/sdcard/`; `..` is rejected. `.ogg` is appended when omitted; `.mp3` files
must include the extension so the native-rate path is selected.

For synthesized speech, use `claw.speak(text[, timeout_ms])` from the
`lua_module_claw` module instead of `audio`.
