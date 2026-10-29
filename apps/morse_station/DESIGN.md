# 电码站 / Morse Station — app design

A lamp-and-key Morse game for the 1024×600 ESP32-P4 panel. The player is the
night operator of a small signal station: read the target, work the brass key,
and watch the decoder tree light up one branch at a time.

## Brief

- **Purpose**: a fast, replayable touch game that teaches/keys Morse code.
- **Audience**: a maker or visitor at the panel; no keyboard, one finger.
- **Tone**: vintage radio-room print — deep ink green, oxidised brass, phosphor
  mint. Quiet technical texture; one loud moment per input.
- **Memorable element**: the live Morse tree. Every dot/dash walks a branch and
  glows; a wrong turn is visible before the answer is revealed. The second
  physical element is the lever key with its hold meter.
- **Constraints**: 1024×600, one full-screen bitmap, no screenshot API, FAT
  package ≤300 KiB target (an SD package is supported on this firmware build),
  CJK TTF subset in the package.

## Modes

| Mode | Target | Action |
|---|---|---|
| 发报训练 (TX) | the letter is shown large | key its code back |
| 抄收挑战 (RX) | the letter is hidden | watch the signal lamp pulse, key the rhythm back |

Both modes share one telegraph key. **Tap = dot, hold ≥280 ms = dash.** The
hold meter under the lever turns brass at the dash threshold. RX offers one
replay at a −30 % score factor; TX offers an audio hint at the same penalty.
Every correct answer adds a combo multiplier (up to ×2.0 at a 10-streak);
a wrong answer or timeout costs one of the three signal lamps.

Level rises every four telegrams: 1–2 symbol letters → 3 symbol letters → all
26 letters, while the input window shrinks. A round is scored from the
remaining window and the combo. The best score persists in
`/fatfs/claw/data/morse_station_best.txt`.

## Palette

| Token | Hex | Role |
|---|---|---|
| bg | `#040806` | near-black green ink |
| panel | `#07110d` | panels, menu card |
| line | `#173326` | etched tree/wire frame |
| mint | `#6ff0b8` | live signal, correct path |
| mint-dim | `#3f9d76` | quiet signal text |
| brass | `#d9b56d` | target node, key, primary action |
| brass-dim | `#7a5f30` | frame brackets, secondary brass |
| cream | `#f2ecda` | display serif, key/score |
| muted | `#7c9488` | body labels |
| danger | `#ff6b5c` | wrong branch, lost lamp |
| amber | `#ffb454` | receive cue, replay state |

The display face is Noto Serif SC 650; the UI face is Noto Sans SC 560. Both
are bundled subsets; the UI subset covers every character in `main.lua`, while
`display.ttf` only carries A–Z, 0–9, space, `? · − ×` and `电码站…断`.

## Wireframe

```
 0        36                        390       680   806   910     1024
 ┌──────────────────────────────────────────────────────────────────┐
 │ MORSE STATION        ●发报训练      得分     连击    信号           │ 0–84
 │ 电码站 · 7.023 MHz   第03号·LV1    01240    ×1.2    ● ● ●         │
 ├───────────────────────────────┬──────────────────────────────────┤
 │ 解码树               26字母·4层│ 报文                 7.023 MHz   │ 104
 │          root                 │ ┌ target card 252×146 ┐  SEND   │
 │      E ─── I ─── …            │ │         K            │         │
 │      T ─── N ─── K(glow)      │ └──────────────────────┘         │
 │      … 16 leaves              │ 纸带  [−][·][−][ ]               │
 │ 左·点  右−划  路径=当前电码   │ 发报正确 +178   K = − · −        │
 │                               │ 信号窗口 ████████░░  7.6s        │
 ├───────────────────────────────┴──────────────────────────────────┤
 │ [ 发报键   轻点·  按住−   ────────● ]  最佳 命中   提示   菜单    │ 508–592
 └──────────────────────────────────────────────────────────────────┘
```

- Tree panel: `x=28 y=104 w=668 h=396`; console: `x=708 y=104 w=288 h=396`;
  key plate: `x=28 y=508 w=680 h=84`; right controls `x=720…996`.
- Tree root `(362,172)`, 64 px per level, 38 px leaf pitch; node radii
  9/18/16/14/12.
- Target card `726,146 252×146`; tape slots at `748/802/856/910`; timer track
  `726,432 252×8`.
- Menu card `172,64 680×472`; game-over card `242,104 540×392`.

## States

`menu → intro (0.9 s) → input / receive → judge (1.1 s correct, 1.5 s wrong)
→ … → gameover → over overlay`. The receive cue is a timed list of dot/dash
events (170 ms unit, 1/3/1/3 gap ratios) that drives the lamp, the sonar rings,
the card border and the OGG beeps. The player's key path is drawn immediately;
the result paints the player's wrong branch in red and the correct branch in
brass.

## Assets and regeneration

```
assets/
  ui_cn.ttf        105 KiB  Noto Sans SC 560 subset, all Lua copy + ASCII
  display.ttf       33 KiB  Noto Serif SC 650, wordmark / big glyphs
  img/bg.jpg        13 KiB  1024×600 baseline JPEG
  sfx/*.ogg          7 KiB  dot, dash, key, ok, err, start, over (Opus)
  OFL.txt                   Noto / SIL OFL 1.1 notice
```

```bash
py -3 tools/make_morse_fonts.py     # Noto VF -> two TTF subsets
py -3 tools/make_morse_assets.py    # bg.jpg + OGG SFX
py -3 tools/morse_preview.py        # host-side design previews, not screenshots
```

The three `docs/morse_station/preview_*.png` images are Pillow mockups drawn
from the same coordinates; they are for design review only. The panel has no
screenshot API — confirm the final composition on the real display.

## Validate and deliver

```bash
py -3 skills/panel-app-design/scripts/validate_app.py \
  --app-dir apps/morse_station --remote-id morse_station
py -3 skills/panel-app-design/scripts/check_font_coverage.py \
  --ttf apps/morse_station/assets/ui_cn.ttf \
  --lua apps/morse_station/scripts/main.lua
py -3 tools/test_morse_app.py     # Lua 5.4 stub-runtime game-flow smoke test

# FAT (launch survives without a card)
py -3 skills/panel-app-design/scripts/upload_app.py \
  --host http://<device-ip> --app-dir apps/morse_station --restart

# SD (recommended when FAT is tight; card must be inserted before boot)
py -3 skills/panel-app-design/scripts/upload_app.py \
  --host http://<device-ip> --app-dir apps/morse_station --volume sd --restart
```

The launcher scans `/fatfs/claw/data/apps` first and `/sdcard/apps` second at
boot, so the upload needs `--restart` (or a manual reboot).
`launcher.json` is written last by the uploader.

An SD package is launched read-only in place; its fonts, background and SFX all
live inside `/sdcard/apps/morse_station/` and resolve through the same
`app_dir` paths. Delete the FAT copy of the same id before boot, otherwise it
shadows the SD package. The best score stays on FAT at
`/fatfs/claw/data/morse_station_best.txt`, so it survives the move. With no
card inserted before boot, only FAT apps are listed.
