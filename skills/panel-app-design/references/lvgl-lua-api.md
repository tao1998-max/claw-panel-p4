# LVGL Lua API: What Works on This Panel

## Runtime

```lua
local lvgl = require("lvgl")
lvgl.init({ buffer_lines = 40, tick_ms = 5, task_period_ms = 10 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#070a12", pad = 0 })
scr:load()

while not claw.abort_requested() do
  lvgl.process_events(20)
end
lvgl.deinit()
```

- `scr:load()` is required; without it the old system screen remains visible.
- Touch is wired automatically after `lvgl.init(...)`; widgets receive touch
  events without an input handle. Event callbacks receive no arguments — read
  state with `get_property`/`get_value`.
- `lvgl.run({ period_ms = 200 })` blocks and drives the event loop if the App
  does not need its own loop; otherwise call `lvgl.process_events(ms)` yourself.
- Do not block the loop with long work or `while true do end`.
- Use `system.millis()` for animation timing.
- `claw.abort_requested()` becomes true when the launcher stops the App.

## Widgets

Supported factories: `screen`, `create_screen`, `object`, `container`, `label`,
`button`, `image`, `line`, `arc`, `bar`, `slider`, `checkbox`, `switch`,
`dropdown`, `roller`, `keyboard`, `list`, `textarea`, `table`, `buttonmatrix`,
`calendar`, `canvas`, `chart`, `imagebutton`, `led`, `menu`, `msgbox`,
`spangroup`, `spinbox`, `tabview`, `tileview`, `window`, plus the file widgets
`eaf`, `lottie`, `animimg`, `gif` below and `font_load`/`set_default_font`.

Common style keys:

```lua
bg_color, bg_opa, opa, radius, border_color, border_width, pad, pad_row,
pad_column, line_color, line_width, arc_width, text_color, font
```

Colors are `"#rrggbb"` or numbers. `x`, `y`, `w`, `h`, `align`, `pad` are object
options. `w` and `h` must both be positive when setting a size.

Alignment values: `top_left`, `top_mid`, `top_right`, `left_mid`, `center`,
`right_mid`, `bottom_left`, `bottom_mid`, `bottom_right`.

**There is no `text_align` option.** `label` centers through the object's
`align="center"`, or by setting a width and using the label's own centering
behavior. Do not try to use `set_style({text_align=...})`.

Buttons support `text`, `font`, `:on("clicked", fn)`, `:set_text()`, and
`:set_style()`.

### Keyboard and text input

```lua
local ta = lvgl.textarea(scr, {
  w = 420, h = 56, one_line = true, max_length = 40,
  placeholder = "输入内容",
})
local kb = lvgl.keyboard(scr, { mode = "text_lower", textarea = ta })
kb:on("ready", function()
  local value = ta:get_property("text")   -- textarea has no get_text()
  -- use value
end)
```

- `mode`: `text_lower`/`lower`, `text_upper`/`upper`, `special`, `number`.
- `popovers = true` shows key previews; `textarea` attaches the keyboard to
  that textarea.
- LVGL places the keyboard with its default size/alignment; set `w`, `h`, `y`
  and `align` explicitly for a custom split and keep other widgets above it.
- `textarea` only exposes `set_text()`; read the current value with
  `get_property("text")`.
- `ready` fires on the keyboard's OK key, `cancel` on its close key.

## Generic property access

`obj:set_property(name_or_id, value[, selector])` and
`obj:get_property(name_or_id)` call LVGL's `lv_obj_property` API, so a widget
property can be read or written without a dedicated Lua method.

```lua
label:set_property("text", "after")
local text = label:get_property("text")        -- "after"
label:set_property("text_color", "#ff0000")    -- style property by name
local color = label:get_property("text_color") -- 0xff0000
local typed = textarea:get_property("text")    -- read user input
```

- Names resolve through the widget class hierarchy first, then style
  properties; unknown names raise an error.
- Values follow the property type: integer, boolean, precise number, color
  (`0xRRGGBB` or `"#RRGGBB"`), point (`{x, y}` or `{x=..., y=...}`), string,
  LVGL object, or font.
- `set_property` returns `false` for read-only properties; `get_property`
  returns `nil` when the property has no getter.
- Paired int/bool properties accept one value or `{first, second}`; pointer and
  text pairs are rejected.
- Style properties accept an optional third `selector` argument
  (`LV_PART_MAIN | LV_STATE_DEFAULT` = 0 by default).
- Geometry getters (`x`, `y`, `w`, `h`) may be stale until the screen is active
  and a frame has been laid out: call `scr:load()` and let the loop run before
  reading them.
- Requires firmware built with `LV_USE_OBJ_PROPERTY` (name lookup also needs
  `LV_USE_OBJ_PROPERTY_NAME`); otherwise both methods raise
  `not enabled in firmware`.

## Layout, flags and events

### Common object methods

- `obj:set_pos(x, y)` / `obj:get_pos()` -> `x, y`
- `obj:set_size(w, h)` / `obj:get_size()` -> `w, h`
- `obj:align(name[, x, y])`
- `obj:set_style(opts)`
- `obj:on(event, fn)` -> handle; `obj:off([handle_or_event])` -> removed count
- `obj:add_flag(name)` / `obj:clear_flag(name)`; flags: `CLICKABLE`,
  `PRESS_LOCK`, `SCROLLABLE`, `HIDDEN`
- `obj:move_foreground()`, `obj:delete()`, `obj:clean()`

Event names: `clicked`, `pressed`, `released`, `long_pressed`,
`value_changed`, `focused`, `defocused`, `ready`, `cancel`. `"click"` is not an
event name; the App fails to load if you use it.

### Flex

```lua
card:set_flex({ flow = "row", main = "space_between",
                cross = "center", track = "start" })
```

- `flow`: `row`, `column`, `row_wrap`, `row_reverse`, `row_wrap_reverse`,
  `column_wrap`, `column_reverse`, `column_wrap_reverse`
- `main` / `cross` / `track`: `start`, `center`, `end`, `space_between`,
  `space_around`, `space_evenly`

### Grid

```lua
grid:set_grid({ cols = {"fr", "fr"}, rows = {"content", 40},
                col_align = "stretch", row_align = "start" })
grid:set_grid_cell({ col = 1, row = 1, col_span = 1, row_span = 1,
                     col_align = "stretch", row_align = "stretch" })
```

### Scrolling

```lua
list:set_scroll({ dir = "ver", scrollbar = "auto",
                  snap_x = "none", snap_y = "none" })
```

- `dir`: `none`, `left`, `right`, `top`, `bottom`, `hor`, `ver`, `all`
- `scrollbar`: `auto`, `off`, `on`, `active`
- `snap_x` / `snap_y`: `none`, `start`, `end`, `center`

For a scrolling list, give the container an explicit `h`, set `dir = "ver"`,
lay the rows out with flex `column`, and clear `SCROLLABLE` on decorative
children so gestures reach the container.

## Type-specific methods

- `label`/`button`/`checkbox`/`dropdown`/`textarea`/`list_text`/`list_button`:
  `set_text(text)`
- `bar`/`slider`/`roller`/`arc`/`scale`/`spinbox`: `set_value(v)`,
  `get_value()`; `set_range(min, max)` for bar/slider/arc/scale/spinbox
- `list`: `add_text(text)` -> list_text, `add_button(text[, symbol])` ->
  list_button
- `table`: `set_cell(row, col, text)`, `get_cell(row, col)`
- `buttonmatrix`: `set_map`, `set_selected`, `get_selected`, `get_button_text`,
  `set_one_checked`
- `calendar`: `set_today`, `set_shown`, `set_highlighted`, `get_pressed_date`
- `chart`: `add_series(color[, axis])`, `set_point_count`, `set_range`,
  `set_next_value(series, v)`, `set_series_values(series, values)`, `refresh()`
- `imagebutton`: `set_src(state, mid[, left, right])`, `set_state(state)`
- `line`: `set_points({{x=0,y=0}, {x=20,y=20}, ...})`
- `led`: `set_color`, `set_brightness`, `on`/`off`/`toggle`
- `menu`: `page`/`cont`/`section`/`separator`, `set_page`, `set_sidebar_page`,
  `set_mode_header`
- `lvgl.canvas`: `fill_bg`, `set_px`, `set_rgb565_data`, `get_px`; for chart-like
  drawing the `image.new` + `set_frame` path above is the faster zero-copy
  option
- `screen`: `load()`; `lvgl.screen()` returns the active screen

## Images

Create an object with a size, then hand it a frame (preferred path):

```lua
local ok, image = pcall(require, "image")
local obj = lvgl.image(scr, { x = 0, y = 0, w = 1024, h = 600, pad = 0 })
local frame <close> = image.load_file(path)  -- .jpg/.jpeg only
obj:set_frame(frame)   -- decode + RGB565 conversion once; zero pixel copy
```

- `set_frame(frame)` retains the image store; releasing the frame afterwards is
  safe. Prefer it over `frame:data()` + `set_raw_rgb565()` (which copies 1.2 MB
  into a Lua string and again into LVGL).
- `set_raw_rgb565(w, h, data[, {swapped=true}])`: little-endian RGB565, for
  bytes you already hold (e.g. `io.read` of a raw file).
- `set_raw_argb8888(w, h, data)`: bytes are `B,G,R,A`, for transparent sprites.
- `data` length must be exactly `w*h*bytes_per_pixel`.
- The object is not scaled to a different source size. Use matching dimensions.
- `set_rotation(angle)` takes tenths of a degree; `set_pivot(x,y)`. There is no
  scale/zoom API in this binding.
- In the `image` module, `image.load_file()` reads baseline JPEG, <=4 MiB,
  <=1920x1080; that module does not decode PNG/GIF/WebP. Pass
  `{ max_width=..., max_height=... }` to decode at a 1/2..1/8 decoder stage
  for thumbnails. For a PNG file source use `lvgl.image{src="D:/..."}` or
  `lvgl.image{src="S:/..."}` instead (LODEPNG is enabled).
- `image.load_raw(path, w, h[, image.RGB565])` reads tightly packed RGB565 or
  GRAY8 pixels. It is optional: if a dark JPEG shows visible chroma
  blocks/banding, generate a raw file with `scripts/make_raw_background.py`
  (ffmpeg decode + Bayer dither) and call `img:set_frame(frame)`. On the
  reference weather App the difference was not visible, so JPEG +
  `set_frame` remains the default.
- `lvgl.image{src="..."}` and `imagebutton` use the LVGL FS: `D:/...` (FAT) or
  `S:/...` (SD, card must be mounted). The decoders enabled in this firmware
  (LODEPNG/TJPGD) decode PNG and JPEG sources from either drive; the `image`
  module's `load_file` remains JPEG/raw-only. GIF animation uses `lvgl.gif`
  (also `D:`/`S:`), not `lvgl.image`.

For a transparent sprite, read the raw file and pass it:

```lua
local f = assert(io.open("/sdcard/panel-assets/id/img/sigil.argb", "rb"))
local data = assert(f:read("*a"))
f:close()
assert(#data == 180 * 180 * 4)
local sprite = lvgl.image(scr, { x = 422, y = 142, w = 180, h = 180 })
sprite:set_raw_argb8888(180, 180, data)
```

## EAF animation

```lua
local f = io.open(path, "rb")
local bytes = f:read("*a")
f:close()
local anim = lvgl.eaf(scr, { src_data = bytes, loop_count = -1, loop_enabled = true })
```

- `src_data` is copied by the binding; Lua bytes and the C copy coexist.
- `src=` uses the LVGL FS: `D:/...` (FAT) or `S:/...` (SD); `src_data` also works.
- Methods: `restart`, `pause`, `resume`, `is_loaded`, `get_total_frames`,
  `get_current_frame`, `set_loop_count`/`get_loop_count`,
  `set_loop_enabled`/`get_loop_enabled`, `set_frame_delay`/`get_frame_delay`.
- Keep the animated region small; full-screen EAF is not practical.

## Lottie animation

`lvgl.lottie` wraps LVGL's ThorVG-based Lottie widget. It renders JSON vector
animations into an ARGB8888 buffer sized `w` x `h`.

```lua
local anim = lvgl.lottie(scr, {
  src = SD .. "anim/loader.json",  -- or src_data = json_string
  w = 160,
  h = 160,
  align = "center",
})
```

- `src` is opened with `fopen` inside ThorVG: unlike `lvgl.image{src}` and
  `lvgl.eaf{src}`, an absolute `/sdcard/...` path **is** allowed.
- `src_data = json_string` also works; ThorVG copies the JSON.
- `autoplay = false` creates the animation paused.
- Methods: `set_src`, `set_src_data`, `pause`, `resume`, `restart`,
  `is_loaded`, `is_playing`, `get_duration` (ms), `get_total_frames`,
  `get_time` (ms).
- Loops forever (finite repeat counts are not exposed).
- Each animation costs `w*h*4` bytes of PSRAM (160x160 = 100 KiB) and renders
  in software on the LVGL task, so keep it small. Requires firmware with
  `LV_USE_LOTTIE`/ThorVG; otherwise it raises `not enabled in firmware`.

## Image sequences (animimg)

`lvgl.animimg` cycles through still images from the LVGL file system.

```lua
local anim = lvgl.animimg(scr, {
  src = {"D:/apps/demo/assets/f0.png",
         "D:/apps/demo/assets/f1.png",
         "D:/apps/demo/assets/f2.png"},
  duration = 900,      -- total cycle time in ms
  repeat_count = -1,   -- -1 forever, 0 once, n repeats
  align = "center",
})
anim:set_src({"D:/apps/demo/assets/f0.jpg", ...})
anim:start(); anim:stop()
```

- Sources are LVGL FS paths: `D:/...` (FAT) or `S:/...` (SD). PNG, JPEG and raw
  LVGL `.bin` all work and may be mixed.
- The binding keeps the source array/strings alive in C.
- `set_duration`/`set_repeat_count` apply on the next `start()`.
- Methods: `set_src`, `start`, `stop`, `set_duration`, `get_duration`,
  `set_repeat_count`, `get_repeat_count`, `get_src_count`.

## GIF

`lvgl.gif` plays animated GIFs with LVGL's built-in decoder.

```lua
local gif = lvgl.gif(scr, {
  src = "D:/apps/demo/assets/anim.gif",
  color_format = "rgb565",  -- default argb8888
  auto_pause = true,
})
gif:pause(); gif:resume(); gif:restart()
```

- `D:/...` (FAT) or `S:/...` (SD) path, same as the other LVGL file sources.
- `color_format` accepts `argb8888`, `rgb565`, `rgb888`, `xrgb8888`;
  `rgb565` halves the decoded frame buffer.
- Methods: `set_src`, `restart`, `pause`, `resume`, `is_loaded`,
  `set_loop_count`, `get_loop_count`, `get_frame_count`, `get_current_frame`,
  `set_auto_pause`, `get_size`.
- Decoding is CPU-heavy; keep the region small and prefer EAF/Lottie for
  full-screen or long loops.

## Numbers in options

Every geometry option (`x`, `y`, `w`, `h`, `pad`, `radius`, `border_width`,
`line_width`, ...) must be an **integer**. Lua 5.4+ `/` always produces a
float, so `y = CH_Y + CH_H / 2` fails with
`lvgl option 'y' must be an integer`; use `CH_H // 2` or `math.floor(...)`.
Float colours/text are fine; only the integer-typed options are strict.

## Images: canvas and zero-copy display

`image.new(width, height, image.RGB565 | image.GRAY8)` creates a writable
canvas; `fill`, `fill_rect`, `line` and `blit` draw into it in C. This replaces
per-pixel Lua string building for charts, gradients and rounded panels:

```lua
local canvas <close> = image.new(920, 320, image.RGB565)
canvas:fill("#0a1422")
canvas:fill_rect(20, 20, 200, 120, 0x3366ff)
canvas:line(20, 300, 900, 40, "#ffcc44", 2)
canvas:blit(photo, 600, 40, 160)          -- JPEG source is converted on the fly

local img = lvgl.image(scr, { x = 20, y = 90, w = 920, h = 320 })
img:set_frame(canvas)                     -- zero copy, keeps the buffer alive
```

- `blit` accepts any `image.frame` (JPEG included), clips and can blend with an
  opacity 0-255.
- Opaque `fill`/`fill_rect` (no opacity, or 255) paint a row and `memcpy` it
  (full 1024x600 fill ~19 ms); per-pixel alpha blending is ~3.4x slower, so use
  opaque fills for backgrounds.
- `set_frame` converts to RGB565 once and hands the buffer to LVGL without
  copying; 100 calls cost about 16 ms. Do not follow it with `set_raw_rgb565`.
- Conversion results are cached per frame, so convert once and reuse.
- Loaded frames are read-only; only `image.new` canvases accept drawing calls.

### Reusing a canvas for a live chart

Create the canvas once, redraw it in C on each update and hand the same buffer
to the same image object. Never rebuild rows with `string.rep`/`string.sub`/
`table.concat`, and never round-trip through `frame:data()`:

```lua
local chart <close> = image.new(920, 320, image.RGB565)
local chart_obj = lvgl.image(scr, { x = 64, y = 104, w = 920, h = 320, pad = 0 })

local function redraw(points)
    chart:fill(0x0a0f17)                       -- opaque: memcpy fast path
    chart:fill_rect(0, 80, 920, 1, 0x18222f)   -- grid
    for x = 0, #points - 1 do
        chart:fill_rect(x, points[x + 1], 2, 3, 0xff3b4e) -- curve segment
    end
    chart_obj:set_frame(chart)                 -- zero copy, same buffer
end
```

- The canvas stays alive for the App; `set_frame` keeps its store referenced,
  so `redraw()` can reuse it for every tap/refresh.
- Calling `set_frame` again with the same buffer invalidates the widget.
- A 920x320 intraday chart (grid + area fill + curve + trailing marker) takes
  about 50 ms on the P4 with a few thousand `fill_rect` calls; the old per-row
  string builder was the hot spot.
- Do not allocate a new canvas per frame; 920x320 RGB565 is 588 KiB.

## JPEG quality, thumbnails and transforms

```lua
-- Re-encode with explicit quality (defaults: quality 80, subsample "422")
image.save_file(path, canvas, { quality = 60, subsample = "420" })
local jpeg <close> = image.encode(canvas, { quality = 90, subsample = "444" })

-- Thumbnail: decode directly at 1/2..1/8, no full-size RGB565 buffer
local thumb <close> = image.load_file(path, { max_width = 320, max_height = 240 })
-- thumb:info().pixel_format == "RGBP"; check actual dimensions (best effort)

-- Resize a JPEG that was not decoded yet: 640x480 -> 160x120 in ~46 ms
local small <close> = image.resize(jpeg_frame, { width = 160, height = 120, filter = "bilinear" })

-- Hardware transforms (new independent frames; source unchanged)
local crop   <close> = image.crop(canvas, 10, 20, 100, 50)  -- x, y, w, h
local turned <close> = image.rotate(canvas, 90)             -- clockwise 90/180/270
local mirror <close> = image.flip(canvas, "horizontal")     -- or "vertical"
```

- `subsample` is `"444"`, `"422"`, `"420"` or `"gray"` (gray only for GRAY8).
- `max_width`/`max_height` are upper bounds; the JPEG decoder only has exact
  1/2, 1/4, 1/8 stages with 8-pixel-aligned output. If no stage fits, you get
  the full-size image, so always read `frame:info()` instead of assuming.
- `image.resize()` on an undecoded JPEG uses the decode stage first, then PPA
  for the remainder; RGB565 sources use PPA directly with `filter="bilinear"`.
- `crop/rotate/flip` use PPA hardware with a CPU fallback; rotation on an
  undecoded JPEG uses the decoder's rotation stage (avoids the intermediate
  unrotated buffer) and falls back to decode + PPA for non-8-aligned sizes.
- In the `image` module PNG is not supported; use a baseline JPEG, an RGB565
  canvas, raw bytes, or `lvgl.image{src=...}` for a PNG file source.

## Fonts

```lua
local font = lvgl.font_load(app_dir .. "/assets/ui_cn.ttf",
                            { size = 20, cache_size = 128 })
lvgl.set_default_font(font)
label:set_style({ font = font })
```

- `font_load` accepts a relative/DATA path, a `D:apps/<id>/...` path, or an
  absolute path under the SD mount (`/sdcard/...`) on this firmware build.
  Older firmware rejects the absolute form.
- Bundle a TTF subset in the package (FAT or SD). The device chat font is not an
  App font and its coverage is not guaranteed.
- Built-in fonts may be referenced by name without `font_load`:
  `montserrat_14` (default), `montserrat_20`, `montserrat_24`, `montserrat_32`,
  `montserrat_48` (ASCII only). Example: `label:set_style({ font = "montserrat_32" })`.
  `text_font` is accepted as an alias of `font`; unknown names are ignored.
- Do **not** call `font:delete()` before `lvgl.deinit()`. Live labels keep font
  pointers; deleting first causes an instruction-access fault on exit.
- Let `lvgl.deinit()` release fonts/widgets together.

## Other modules

- `audio` provides output/input/player/recorder/analyzer plus the one-shot
  `audio.play("click")` shortcut; see [`audio.md`](audio.md). It uses the shared
  XiaoZhi audio service, so output is rejected while the assistant is active.
  Local and `http(s)://` MP3 playback pauses that service and takes the codec
  over at the source's native rate; see the MP3/stream section in `audio.md`.
- `storage.exists/stat/read_file/write_file/mkdir/remove/remove_tree/rename/
  listdir` accept absolute FAT/SD paths (the App job owns them); only
  `get_free_space()` reports FAT.
- `system.millis()` for timing; `delay.delay_ms(ms)` yields to other tasks;
  `claw.abort_requested()` for exit.
- `net`, `mqtt` and `websocket` provide networking; see
  [`networking.md`](networking.md).

## Not available

- screenshot/vision or pixel probing;
- hardware encoder/keypad input (touch and the on-screen `keyboard` only);
- chart cursors and other advanced chart APIs; spangroup span and chart series
  handles are not full widgets (no `set_pos` etc.);
- WebP/BMP/SVG decoding (PNG/JPEG file sources and animated GIFs are available
  through LVGL widgets);
- a frame-by-frame animation API beyond EAF, Lottie, `animimg` and `gif`
  (animate positions or opacity in the loop otherwise);
- LVGL widget zoom/scale (scale the pixels with `image.resize` or decode-time
  scaling, then `set_frame`);
- reading `/sdcard/...` directly in LVGL `src` strings: use the `S:/...` drive
  for LVGL widgets (Lottie is the exception: it takes absolute `/sdcard/...`
  because ThorVG bypasses LVGL's FS);
- launching a second UI App while one is running.
