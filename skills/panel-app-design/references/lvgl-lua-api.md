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
- Do not block the loop with long work or `while true do end`.
- Use `system.millis()` for animation timing.
- `claw.abort_requested()` becomes true when the launcher stops the App.

## Widgets

Supported: `container`, `label`, `button`, `image`, `line`, `arc`, `bar`,
`slider`, `checkbox`, `switch`, `dropdown`, `roller`, `textarea`, `table`,
`chart`, `canvas`, `led`, `spinner`, `tabview`, `tileview`, `menu`, `msgbox`,
and more.

Common style keys:

```lua
bg_color, bg_opa, opa, radius, border_color, border_width, pad, text_color, font
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

## Images

Create an object with a size, then upload pixels:

```lua
local ok, image = pcall(require, "image")
local frame = image.load_file(path)          -- .jpg/.jpeg only
local rgb = image.convert(frame, image.RGB565)
local data = rgb.data()
frame:release()
rgb:release()

local obj = lvgl.image(scr, { x = 0, y = 0, w = 1024, h = 600, pad = 0 })
obj:set_raw_rgb565(1024, 600, data)
```

- `set_raw_rgb565(w, h, data[, {swapped=true}])`: little-endian RGB565.
- `set_raw_argb8888(w, h, data)`: bytes are `B,G,R,A`.
- `data` length must be exactly `w*h*bytes_per_pixel`.
- The object is not scaled to a different source size. Use matching dimensions.
- `set_rotation(angle)` takes tenths of a degree; `set_pivot(x,y)`. There is no
  scale/zoom API in this binding.
- `image.load_file()` reads baseline JPEG, <=4 MiB, <=1920x1080. PNG/GIF/WebP
  are not decoded at runtime.
- `lvgl.image{src="..."}` and `imagebutton` use the LVGL FS (`D:` under the
  Claw data root); they cannot read `/sdcard` or JPEG. Use raw setters instead.

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
- `src=` uses the LVGL FS and cannot read SD; use `src_data`.
- Methods: `restart`, `pause`, `resume`, `is_loaded`, `get_total_frames`,
  `get_current_frame`, `set_loop_count`, `set_loop_enabled`, `set_frame_delay`.
- Keep the animated region small; full-screen EAF is not practical.

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
- Do **not** call `font:delete()` before `lvgl.deinit()`. Live labels keep font
  pointers; deleting first causes an instruction-access fault on exit.
- Let `lvgl.deinit()` release fonts/widgets together.

## Other modules

- `audio.play("click")` resolves `<app>/assets/sfx/click.ogg`; absolute
  `/sdcard/...` OGG is allowed. It is a shared one-shot player, not a gapless
  BGM engine.
- `storage.exists/read_file/listdir/stat` accept absolute SD paths, but
  `get_free_space()` is FAT-only.
- `system.millis()` for timing; `claw.abort_requested()` for exit.
- `net`, `mqtt` and `websocket` provide networking; see
  [`networking.md`](networking.md).

## Not available

- screenshot/vision or pixel probing;
- PNG/GIF/WebP/BMP/SVG decoding;
- a general animation API (animate by changing positions/opacity in the loop);
- image scaling/zoom;
- reading SD through `lvgl.image{src}` or `lvgl.eaf{src}` (fonts are the
  exception: absolute `/sdcard` paths work);
- launching a second UI App while one is running.
