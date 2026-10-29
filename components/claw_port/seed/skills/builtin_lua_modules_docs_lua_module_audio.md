# audio

Audio output, microphone input, recording, analysis, and short OGG playback
through the on-board ES8311. Load it with `require("audio")`.

The module uses the XiaoZhi `AudioService` that already owns the codec/I2S.
It never opens a second codec. While XiaoZhi is connecting, listening, or
speaking, output is rejected so game audio never talks over the assistant.
Recording/analysis reads the same 10 ms frames that feed WakeNet/AFE, so it
does not create a second microphone reader.

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
`close`. Only one output may be open.

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
local state = player:poll() -- { state = "playing"|"finished"|"stopped"|"idle", ... }
player:stop()
player:close()
```

This version plays local OGG/Opus files only. `pause`/`resume` return
`nil, "unsupported"`; HTTP/MP3/AAC playback is not implemented.

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
audio.play("/sdcard/effects/push.ogg")
audio.is_available()
```

App-relative paths are resolved from the running script's app root. Allowed
absolute roots are `/fatfs/claw/data/`, `/fatfs/claw/system/`, `/fatfs/ai/`,
and `/sdcard/`; `..` is rejected. `.ogg` is appended when omitted.
