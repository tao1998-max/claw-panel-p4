# Audio (Lua)

## Speech synthesis (TTS)

Arbitrary text can be spoken with the Claw runtime's `claw.speak()`:

```lua
local claw = require("claw")
local ok, err = claw.speak("温度 26 度，湿度 48%", 30000)
if not ok then
    claw.log("TTS failed: " .. tostring(err))
end
```

- `claw.speak(text[, timeout_ms])` uses Doubao TTS and the same ES8311 output.
  It is an **explicit** call: the device's “自动播报” switch only controls
  automatic playback of Agent replies, not this function.
- Requires TTS credentials configured in **Claw → 设置 → 豆包语音** (or
  `POST /api/tts/config`). Never put API keys in an App package.
- Blocks the Lua task until playback finishes: default 120 s, maximum 600 s.
  Use short phrases; do not call it from an animation/timer loop.
- Speech is one streaming session per call. Emoji/pictographs are stripped
  before synthesis, so text containing 👋/😊 still plays.
- Returns `true`, or `nil, err` when TTS is not configured, the shared audio
  path is busy (assistant speaking), or synthesis fails.
- This is different from `audio.play()`: use `claw.speak` for synthesized
  speech; use `audio` for bundled OGG/MP3 effects and music.

`require("audio")` wraps the XiaoZhi `AudioService` (ES8311 codec + I2S). There
is only one audio owner: while the assistant is connecting, listening or
speaking, PCM writes and OGG playback are rejected with
`nil, "audio output busy or assistant is active"`. `audio.is_available()`
reports whether that service is running. The module never opens a second codec,
and only one output and one input may be open at a time.

MP3 is the exception: playback at the file's native rate **pauses** the
assistant's AudioService and reconfigures the shared ES8311/I2S, then restores
16 kHz and restarts the assistant when playback ends. Wake word / voice is
unavailable while an MP3 plays.

## Quick start

```lua
local audio = require("audio")

if audio.is_available() then
    audio.play("click")                    -- <app>/assets/sfx/click.ogg
end

local out = audio.open_output({ volume = 60 })
out:play_tone(880, 120)                    -- 880 Hz, 120 ms
out:write(pcm)                             -- 16 kHz mono 16-bit, even length
out:close()
```

## Module functions

| Function | Returns | Notes |
|---|---|---|
| `audio.open_output([{volume=0..100}])` | output | one output at a time; default volume unchanged |
| `audio.open_input([opts])` | input | opts `sample_rate=16000`, `channels=1`, `bits=16`, `volume=0..100`; only 16 kHz mono 16-bit; unavailable if the mic is muted or the assistant is disabled |
| `audio.player({output=out})` | player | local OGG/Opus and native-rate MP3 with `wait`/`poll`/`stop` |
| `audio.recorder({input=in})` | recorder | records 16 kHz mono WAV |
| `audio.analyzer({input=in})` | analyzer | level meter and FFT spectrum |
| `audio.play(name)` | boolean | fire-and-forget OGG/Opus/MP3; false when unavailable |
| `audio.is_available()` | boolean | XiaoZhi audio service is running |

## Output methods

- `info()` -> `{sample_rate=16000, channels=1, bits=16}`
- `set_volume(0..100)`, `get_volume()`
- `set_mute(bool)` — accepted but a no-op on the shared output
- `write(pcm)` -> bytes written, or `nil, err`; PCM must be non-empty,
  byte-aligned (16-bit) and takes up to 2 s to drain
- `play_tone(freq_hz, duration_ms)` -> `true`/`nil, err`; frequency 1..7999 Hz,
  duration 1..5000 ms, 5 ms fade in/out
- `close()`

## Input methods

- `info()` -> `{sample_rate=16000, channels=1, bits=16}`
- `set_volume(0..100)`, `get_volume()`
- `read(bytes | {bytes=n})` -> PCM string, or `nil` on timeout; reads from the
  same 10 ms frames that feed WakeNet/AFE
- `close()`

## Player methods

- `play(name [, {wait=false}])` -> `true` or `nil, err`; local OGG/Opus,
  local `.mp3`, or an `http://` / `https://` MP3 stream. With `wait=true` it
  blocks until playback finishes (60 s cap; live streams do not finish).
- `stop()`
- `poll()` -> `{state="buffering"|"playing"|"finished"|"stopped"|"idle"|"error",
  running=bool, level=0..1, error?,
  music_info={sample_rate, channels, bits, bitrate, source_sample_rate,
  source_channels}}`
- `pause()` / `resume()` -> `nil, "unsupported by the shared AudioService player"`
- `close()`

## MP3 and radio streams (native rate)

```lua
local player = audio.player({ output = out })
player:play("/sdcard/music/song.mp3", { wait = false })
player:play("http://ice1.somafm.com/groovesalad-128-mp3", { wait = false })
local ev = player:poll()
-- 44.1k stereo: ev.music_info.sample_rate == 44100, channels == 2
-- ev.level is the RMS of the last decoded frame (0..1), for visualizers
player:stop()
```

- Plays at the source's native rate (8–48 kHz) and channels; mono is duplicated
  to stereo. Decoding runs on a background task so the UI keeps running.
- URLs support HTTP(S), redirects and chunked transfer; ICY metadata is not
  requested, so the body must be MP3. AAC/HLS streams are rejected.
- A dropped stream reconnects automatically (up to 8 attempts) and reports
  `state="buffering"`; call `stop()` to end it.
- Refuses to start while the assistant is connecting/listening/speaking; retry
  when it is idle. **Wake word and voice are unavailable during playback.**
- On EOF, `stop()`, `close()` or App exit the codec returns to 16 kHz and the
  assistant restarts. Starting another MP3/stream stops the first.
- `music_info.sample_rate/channels` are the actual output format;
  `source_sample_rate/source_channels` describe the decoded source. `state ==
  "error"` includes an `error` string.
- `audio.play("x.mp3")` / `audio.play("http://...")` is fire-and-forget; it
  stops on App exit or EOF. Use `audio.player` when you need `stop()`/`poll()`.
- Input/recorder/analyzer are unavailable while MP3/radio plays (the assistant's
  input path is paused).

See `assets/examples/network_radio/` for a complete radio App: station cards,
`player:poll()` state/level, a canvas spectrum and Chinese UI.

## Recorder methods

```lua
local rec = audio.recorder({ input = input })
local meta = rec:record("memo", { duration_ms = 1000 })
-- { path=..., duration_ms=..., bytes=..., format="wav" }
rec:close()
```

- `duration_ms` is 1..60000 (default 5000).
- Relative names with `/` resolve under the App root; a bare name goes to
  `<app>/recordings/<name>.wav`; `.wav` is appended when missing.
- Absolute paths must be under `/fatfs/claw/data/` or `/sdcard/`.
- Returns `nil, err` if the input times out or storage fails.

## Analyzer methods

- `read_level([duration_ms | {duration_ms=}])` -> `{rms=0..1, peak=0..32767,
  duration_ms}`; duration 1..1000 ms (default 100)
- `read_spectrum([fft_size], [bands] | {fft_size=, bands=})` ->
  `{bands={0..255,...}, peak_freq_hz, peak_db, rms, fft_size, band_count,
  sample_rate=16000}`; `fft_size` is a power of two 64..4096, `bands` is
  1..fft_size/2. Hann window, -90..-20 dB mapped to 0..255.
- `close()`

## Paths

- App-relative: `audio.play("deal")` -> `<app>/assets/sfx/deal.ogg`;
  `audio.play("sfx/win")` -> `<app>/assets/sfx/win.ogg`; explicit paths such as
  `assets/sfx/win.ogg` also work. `.ogg` is appended when omitted; `.mp3` must
  include the extension.
- Absolute playback roots: `/fatfs/claw/data/`, `/fatfs/claw/system/`,
  `/fatfs/ai/`, `/sdcard/`. `..` is rejected.
- `http://` / `https://` URLs bypass the app-root rules and are fetched
  directly (MP3 only).
- Recording roots: `/fatfs/claw/data/`, `/sdcard/` only.

## Limits / not implemented

- PCM output, tones, OGG/Opus and recording are 16 kHz mono 16-bit. MP3/stream
  output is native 8–48 kHz, 16-bit stereo (mono duplicated).
- Local OGG/Opus files and local/HTTP(S) MP3 only: AAC/HLS playback is not
  implemented; no seek, pause/resume or per-effect volume.
- MP3/radio pauses the assistant AudioService: wake word/voice and input/
  recorder/analyzer are unavailable while it plays. One stream at a time.
- Recording and analysis share the microphone tap with WakeNet/AFE; expect
  short reads under load.
