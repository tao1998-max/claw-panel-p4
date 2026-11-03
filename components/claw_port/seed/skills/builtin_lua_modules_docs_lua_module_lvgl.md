# Lua LVGL Usage Guide

This document is an API guide for building LVGL UIs from Lua with the `lvgl` module.

## Core Rules

- Import the module with `local lvgl = require("lvgl")`.
- Call `lvgl.init([opts])` before creating screens or widgets.
- All widget operations are userdata methods. Use `btn:set_text("OK")`, not `lvgl.set_text(btn, "OK")`.
- Only one Lua script can own the LVGL runtime at a time. Do not use `display.open(...)` and `lvgl.init(...)` together.
- Call `lvgl.deinit()` before the script exits. The module also cleans up automatically if the owner script exits unexpectedly.
- Object handles become invalid after `obj:delete()`, after a parent object is deleted, or after `lvgl.deinit()`.
- After registering events, call `lvgl.run()` or repeatedly call `lvgl.process_events(...)`; otherwise Lua callbacks will not run.

## Minimal Example

```lua
local lvgl = require("lvgl")

lvgl.init({
    buffer_lines = 40,
    tick_ms = 5,
    task_period_ms = 10,
})

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#101820" })

local label = lvgl.label(scr, {
    text = "LVGL from Lua",
    align = "top_mid",
    y = 20,
    text_color = "#ffffff",
})

local btn = lvgl.button(scr, {
    text = "OK",
    align = "center",
    w = 120,
    h = 44,
    bg_color = "#2f80ed",
    text_color = "#ffffff",
})

btn:on("clicked", function()
    label:set_text("clicked")
end)

scr:load()
lvgl.run()
lvgl.deinit()
```

## Init And Deinit

```text
lvgl.init([opts]) -> true
```

Common `opts`:
- `buffer_lines`: draw buffer height in lines, default `40`
- `tick_ms`: LVGL tick period, default `5`
- `task_period_ms`: LVGL handler task period, default `10`

Legacy scripts may still call `lvgl.init(panel_handle, io_handle, width, height, panel_if, opts)`. The positional display parameters are ignored; only `opts` is used.

Legacy constants are still exported for older scripts:

- `lvgl.PANEL_IF_IO`
- `lvgl.PANEL_IF_RGB`
- `lvgl.PANEL_IF_MIPI_DSI`

They are not needed by `lvgl.init([opts])`.

Shutdown:

```text
lvgl.deinit() -> true
```

## Touch Input

The board's LCD touch device is connected automatically. After `lvgl.init(...)`, widgets receive touch events without registering an input handle.

## Event Loop

Register callbacks with `obj:on(event, callback)`:

```lua
local handle = btn:on("clicked", function()
    print("clicked")
end)
```

Remove callbacks:

```lua
btn:off(handle)      -- -> removed_count
btn:off("clicked")  -- -> removed_count
btn:off()           -- -> removed_count
```

Supported event names:

`clicked`, `pressed`, `released`, `long_pressed`, `value_changed`,
`focused`, `defocused`, `ready`, `cancel`

Drive the event loop:

```text
lvgl.run([{ period_ms = 200 }]) -> processed_count
```

Or:

```lua
while true do
    lvgl.process_events(50) -- -> processed_count
    -- run other periodic Lua-side work here
end
```

`lvgl.process_events([timeout_ms = 0])` treats negative timeouts as `0`. Event callbacks receive no arguments; callback errors are logged and event dispatch continues.

## Widget Constructors

All constructors follow the same basic shape:

```lua
local obj = lvgl.widget(parent, opts) -- -> object userdata
```

Basic widgets:

- `lvgl.object(parent, opts)`
- `lvgl.container(parent, opts)`
- `lvgl.label(parent, opts)`
- `lvgl.button(parent, { text = "OK" })`
- `lvgl.bar(parent, { min = 0, max = 100, value = 50 })`
- `lvgl.slider(parent, { min = 0, max = 100, value = 50 })`
- `lvgl.arc(parent, { min = 0, max = 100, value = 50 })`
- `lvgl.scale(parent, { min = 0, max = 100, value = 50 })`
- `lvgl.checkbox(parent, { text = "Enable", checked = true })`
- `lvgl.switch(parent, { checked = true })`
- `lvgl.dropdown(parent, { options = {"A", "B"}, selected = 1 })`
- `lvgl.roller(parent, { options = {"A", "B"}, selected = 1 })`
- `lvgl.keyboard(parent, { mode = "text_lower", textarea = textarea })`
- `lvgl.textarea(parent, { text = "..." })`
- `lvgl.list(parent, opts)`
- `lvgl.table(parent, { rows = 2, cols = 2 })`
- `lvgl.image(parent, { src = "<your/path/image.bin>" })`
- `lvgl.line(parent, { points = {{x=0,y=0}, {x=20,y=20}} })`
- `lvgl.spinner(parent, { anim_ms = 1000, arc_sweep = 60 })`
- `lvgl.buttonmatrix(parent, { map = {"1", "2", "\n", "3"}, one_checked = true })`
- `lvgl.calendar(parent, { today = {2026, 5, 15}, shown = {2026, 5}, highlighted = {{2026, 5, 15}} })`
- `lvgl.canvas(parent, { w = 80, h = 40, color_format = "rgb565" })`
- `lvgl.chart(parent, { type = "line", point_count = 10, min = 0, max = 100, update_mode = "shift" })`
- `lvgl.imagebutton(parent, { src = "<your/path/image.bin>" })`
- `lvgl.led(parent, { color = "#00ff00", brightness = 180, on = true })`
- `lvgl.menu(parent, opts)`
- `lvgl.msgbox(parent_or_nil, { title = "...", text = "...", buttons = {"OK"}, close_button = true })`
- `lvgl.spangroup(parent, { mode = "break", overflow = "ellipsis", spans = {"A", "B"} })`
- `lvgl.spinbox(parent, { min = 0, max = 100, value = 10, step = 1 })`
- `lvgl.tabview(parent, { tab_bar_position = "top", tab_bar_size = 36 })`
- `lvgl.tileview(parent, opts)`
- `lvgl.window(parent, opts)`
- `lvgl.eaf(parent, { src = "<your/path/anim.eaf>", loop_count = -1, loop_enabled = true })`
- `lvgl.lottie(parent, { src = "<your/path/anim.json>", w = 200, h = 200, autoplay = true })`
- `lvgl.animimg(parent, { src = {"<path0.png>", "<path1.png>"}, duration = 900, repeat_count = -1 })`
- `lvgl.gif(parent, { src = "<your/path/anim.gif>", color_format = "rgb565" })`

Lua index convention:
- dropdown/roller selected indexes are 1-based
- table rows and columns are 1-based
- buttonmatrix selected indexes are 1-based
- tabview active indexes are 1-based
- tileview `col` and `row` are 1-based

Widget-specific `opts`:

- `image`: `src`
- `line`: `points`, `y_invert`
- `arc`: `start_angle`, `end_angle`, `bg_start_angle`, `bg_end_angle`, `rotation`, `mode`
- `spinner`: `anim_ms`, `arc_sweep`
- `scale`: `mode`, `total_ticks`, `major_tick_every`, `label_show`, `angle_range`, `rotation`
- `checkbox`, `switch`: `checked`
- `dropdown`: `options`, `selected`, `dir`, `symbol`
- `roller`: `options`, `selected`, `mode`, `visible_rows`
- `keyboard`: `mode`, `popovers`, `textarea`
- `textarea`: `placeholder`, `one_line`, `password`, `max_length`, `accepted_chars`
- `table`: `rows`, `cols`, `cells`, `column_widths`
- `buttonmatrix`: `map`, `one_checked`
- `calendar`: `today`, `shown`, `highlighted`, `day_names`
- `canvas`: `w`, `h`, `color_format`
- `chart`: `type`, `point_count`, `min`, `max`, `update_mode`
- `imagebutton`: `src`
- `led`: `color`, `brightness`, `on`
- `msgbox`: `title`, `text`, `buttons`, `close_button`
- `spangroup`: `mode`, `overflow`, `indent`, `max_lines`, `spans`
- `spinbox`: `min`, `max`, `value`, `step`, `digit_count`, `dec_point_pos`, `rollover`
- `tabview`: `tab_bar_position`, `tab_bar_size`
- `eaf`: `src`, `src_data`, `loop_count`, `loop_enabled`, `frame_delay`
- `lottie`: `src`, `src_data`, `w`, `h`, `autoplay`
- `animimg`: `src` (array of LVGL FS paths), `duration`, `repeat_count`, `autoplay`
- `gif`: `src`, `color_format`, `loop_count`, `auto_pause`

## Common Options

Most widgets support:

- Position and size: `x`, `y`, `w`, `h`, `align`
- Text: `text`
- Numeric values: `min`, `max`, `value`
- Style: `bg_color`, `text_color`, `border_color`, `bg_opa`, `opa`,
  `radius`, `border_width`, `pad`, `pad_row`, `pad_column`,
  `line_color`, `line_width`, `arc_width`, `font` (`text_font` is accepted
  as an alias; the value is a font object or a built-in font name string)

Colors can be strings or numbers:

```lua
bg_color = "#2f80ed"
text_color = 0xffffff
```

## Fonts

`lvgl.init()` does not load a font. Load a font explicitly, then apply it to individual controls or set it as the default:

```lua
local lvgl = require("lvgl")

-- Pass an absolute path to a TTF that exists in the package or DATA root.
local font = lvgl.font_load("/fatfs/claw/data/apps/<id>/assets/ui_cn.ttf",
                            { size = 24, cache_size = 128 })
lvgl.set_default_font(font)
label:set_style({ font = font })
```

- `lvgl.font_load(path[, { size = 16, cache_size = ... }])` -> font handle
- `lvgl.set_default_font(font | nil)` -> true; applies to the current root screen and subsequently created screens
- `font:set_size(px)` -> true
- `font:is_valid()` -> boolean
- `font:delete()` -> true

### Built-in fonts (no TTF needed)

The firmware compiles Montserrat sizes 14, 20, 24, 32 and 48. Reference them
directly by name string in the `font` style; ASCII text and digits only:

```lua
local scr = lvgl.create_screen()
lvgl.label(scr, { text = "12:34", font = "montserrat_48", align = "center" })
lvgl.label(scr, { text = "PLANT", font = "montserrat_20" })
```

`montserrat_14` is the default LVGL font and `"default"` is an alias for it.
Built-in Montserrat has **no Chinese glyphs**; for Chinese text load a TTF with
`lvgl.font_load()` as shown above. Keep the returned font object alive while
it is used (do not `font:delete()` it until the widgets are gone).

Without `path`, `lvgl.font_load()` looks for `fonts/NotoSansSC-Regular-sub.ttf`
under the DATA root. **This product does not ship that file**, so always pass a
path to a TTF that exists (an App asset or a file you copied yourself), or keep
the UI ASCII and use the built-in LVGL font. Passing `nil` to
`lvgl.set_default_font()` restores the built-in font. An explicit `font` style
on a control overrides the default font. Each `font_load` keeps the whole TTF
in RAM, so bundle a small subset and reuse sizes where possible.

## EAF Animation

EAF playback is exposed as an LVGL object:

```lua
local anim = lvgl.eaf(scr, {
    src = "<your/path/idle.eaf>",
    align = "center",
    loop_count = -1,
    loop_enabled = true,
    frame_delay = 100,
})

if anim:is_loaded() then
    anim:pause()
    anim:resume()
end
```

Methods:
- `anim:set_src(path)`
- `anim:set_src_data(binary_string)`
- `anim:restart()`, `anim:pause()`, `anim:resume()`
- `anim:is_loaded()`
- `anim:get_total_frames()`, `anim:get_current_frame()`
- `anim:set_loop_count(n)`, `anim:get_loop_count()`
- `anim:set_loop_enabled(boolean)`, `anim:get_loop_enabled()`
- `anim:set_frame_delay(ms)`, `anim:get_frame_delay()`

Use either `src` or `src_data` when creating an EAF object.

## Lottie Animation

Lottie playback uses LVGL's `lv_lottie` widget with the bundled ThorVG software renderer. The widget renders into an ARGB8888 buffer sized `w` x `h`.

```lua
local anim = lvgl.lottie(scr, {
    src = app_dir .. "/assets/loading.json",  -- or src_data = json_string
    w = 200,
    h = 200,
    align = "center",
    autoplay = true,
})

if anim:is_loaded() then
    anim:pause()
    anim:resume()
    anim:restart()
end
```

Methods:
- `anim:set_src(path)` -> boolean (true if the source parsed)
- `anim:set_src_data(json_string)` -> boolean
- `anim:pause()`, `anim:resume()`, `anim:restart()`
- `anim:is_loaded()` -> boolean
- `anim:is_playing()` -> boolean
- `anim:get_duration()` -> single-cycle duration in ms
- `anim:get_total_frames()` -> frame count
- `anim:get_time()` -> elapsed ms in the current cycle

Notes:
- `src` is read with `fopen` inside ThorVG, so pass a VFS path (`/sdcard/...`, `/fatfs/...`); LVGL's `D:` drive prefix is not used.
- The animation loops forever. Finite loop counts are not exposed because LVGL frees a completed animation while `lv_lottie` keeps a pointer to it.
- Rendering is software (ThorVG): each animation occupies `w * h * 4` bytes of PSRAM and large sizes cost CPU per frame.
- ThorVG needs roughly 22 KB of stack per rendered frame; this template raises the LVGL and Lua task stacks to 32 KB.
- Requires firmware built with `LV_USE_LOTTIE` and ThorVG (`LV_USE_THORVG_INTERNAL`); otherwise `lvgl.lottie` raises `not enabled in firmware`.

## Image Sequences And GIF

`lvgl.animimg` cycles through an array of still images; `lvgl.gif` plays an
animated GIF. Both read sources through LVGL's file system:

- `D:/<path>` resolves to the Claw data root, typically `/fatfs/claw/data`
- `S:/<path>` resolves to the SD card mount point, typically `/sdcard`
  (available when a card is mounted)

```lua
local frames = lvgl.animimg(scr, {
    src = {"D:/apps/demo/assets/f0.png",
           "D:/apps/demo/assets/f1.png",
           "D:/apps/demo/assets/f2.png"},
    duration = 900,        -- total cycle duration in ms
    repeat_count = -1,     -- -1 infinite, 0 play once, n repeat n times
    autoplay = true,
})

local gif = lvgl.gif(scr, {
    src = "D:/apps/demo/assets/anim.gif",
    color_format = "rgb565",  -- default argb8888
    auto_pause = true,
})
```

`animimg` methods:
- `anim:set_src(frames)` -> true (replaces the sequence and starts it)
- `anim:start()`, `anim:stop()`
- `anim:set_duration(ms)`, `anim:get_duration()`
- `anim:set_repeat_count(n)`, `anim:get_repeat_count()` -> -1 for infinite
- `anim:get_src_count()` -> integer

`gif` methods:
- `gif:set_src(path)` -> boolean (true if the file opened)
- `gif:restart()`, `gif:pause()`, `gif:resume()`
- `gif:is_loaded()`
- `gif:set_loop_count(n)`, `gif:get_loop_count()`
- `gif:get_frame_count()`, `gif:get_current_frame()`
- `gif:set_auto_pause(boolean)`
- `gif:get_size()` -> `w, h` or nil

Notes:
- PNG, JPEG and GIF decoders are enabled in this firmware. Raw LVGL `.bin`
  images also work; `animimg` sources can mix formats.
- `repeat_count` follows LVGL semantics: `-1` repeats forever, `0` plays once,
  `n > 0` repeats `n` times after the first pass. `set_duration` and
  `set_repeat_count` affect the next `start()`.
- `animimg` keeps its source array and strings alive in C; Lua values may be
  released after construction.
- `gif` `color_format` accepts `argb8888`, `rgb565`, `rgb888`, or `xrgb8888`.
  `rgb565` roughly halves the frame buffer memory.
- Sources are looked up through the module's `D:` (FAT) or `S:` (SD) drive.
  For example, an SD library asset is `S:/panel-assets/<id>/f0.png`.

## Common Methods

All LVGL object userdata supports:

- Methods that change object state return `true` on success unless another return value is shown.
- `obj:set_pos(x, y)`
- `obj:get_pos()` -> `x, y`
- `obj:set_size(w, h)`
- `obj:get_size()` -> `w, h`
- `obj:align(name[, x, y])`
- `obj:is_valid()` -> boolean
- `obj:set_style(opts)`
- `obj:set_flex(opts)`
- `obj:set_grid(opts)`
- `obj:set_grid_cell(opts)`
- `obj:set_scroll(opts)`
- `obj:on(event, callback)` -> handle
- `obj:off([handle_or_event])` -> removed_count
- `obj:set_property(name_or_id, value[, selector])` -> boolean
- `obj:get_property(name_or_id)` -> value or nil
- `obj:delete()`
- `obj:clean()`

Screen helpers:
- `lvgl.screen()` -> active screen object
- `lvgl.create_screen()` -> screen object

Common `align` names:

`top_left`, `top_mid`, `top`, `top_right`, `bottom_left`, `bottom_mid`,
`bottom`, `bottom_right`, `left_mid`, `left`, `right_mid`, `right`,
`center`, `centre`, `default`

## Object Flags And Order

- `obj:add_flag(name)` / `obj:clear_flag(name)` -> true. Supported names: `CLICKABLE`, `PRESS_LOCK`, `SCROLLABLE`, `HIDDEN` (case-sensitive).
- `obj:move_foreground()` -> true. Moves the object above its siblings; screens have no parent and raise an error.

Use `PRESS_LOCK` on hold controls to keep receiving release events when a finger leaves the control. Clear `CLICKABLE` on decorative widgets so they do not intercept touches. `HIDDEN` disables drawing and hit testing without deleting the object. These methods raise errors for unsupported flags or invalid/deleted handles.

## Generic Property Access

`set_property` / `get_property` expose LVGL's `LV_USE_OBJ_PROPERTY` API directly, so any property LVGL implements for a widget can be read or written without a dedicated Lua method. Names are resolved with `lv_obj_property_get_id()`, which searches the widget class hierarchy first and style properties second.

```lua
local label = lvgl.label(scr, { text = "before" })

label:set_property("text", "after")
local text = label:get_property("text")            -- "after"

-- Read back what the user typed into a textarea
local value = textarea:get_property("text")

-- Style properties work by name too
label:set_property("text_color", "#ff0000")
local color = label:get_property("text_color")     -- 0xff0000
```

- `set_property` returns `true` when LVGL accepted the value, `false` for read-only properties.
- `get_property` returns `nil` when the property exists but has no getter.
- Values are converted from the type embedded in the property ID: integer, boolean, precise number, color (`0xRRGGBB` or `"#RRGGBB"`), point (`{x=..., y=...}` or `{x, y}`), string, `lvgl` object, or font.
- `selector` is an optional `lv_part_t | lv_state_t` value for style properties; it defaults to `LV_PART_MAIN | LV_STATE_DEFAULT` (0).
- Paired properties such as `slider.value` accept the first value alone or a `{first, second}` table. Paired values are limited to int/bool types; pointer/text pairs are rejected.
- Geometry getters (`x`, `y`, `w`, `h`) return the last laid-out coordinates. Make the screen active and let the LVGL task run a frame before relying on them, otherwise they may return stale values.
- Unknown names raise an error.
- Requires firmware built with `CONFIG_LV_USE_OBJ_PROPERTY=y`; name lookup additionally needs `CONFIG_LV_USE_OBJ_PROPERTY_NAME=y`. Otherwise both methods raise `not enabled in firmware`.
- Coverage follows LVGL's property tables (`src/widgets/property/`); properties returning raw pointers (image source, display, font) are not converted and `get_property` returns `nil`.

## Layout And Scrolling

Flex:

```lua
obj:set_flex({
    flow = "column",
    main = "start",
    cross = "center",
    track = "start",
})
```

`flow`: `row`, `column`, `row_wrap`, `row_reverse`, `row_wrap_reverse`,
`column_wrap`, `column_reverse`, `column_wrap_reverse`

`main/cross/track`: `start`, `center`, `end`, `space_between`,
`space_around`, `space_evenly`

Grid:

```lua
obj:set_grid({
    cols = {"fr", "fr"},
    rows = {"content", 40},
    col_align = "stretch",
    row_align = "start",
})
```

Grid cell:

```lua
obj:set_grid_cell({
    col = 1,
    row = 1,
    col_span = 1,
    row_span = 1,
    col_align = "stretch",
    row_align = "stretch",
})
```

Scroll:

```lua
obj:set_scroll({
    dir = "ver",
    scrollbar = "auto",
    snap_x = "none",
    snap_y = "none",
})
```

`dir`: `none`, `left`, `right`, `top`, `bottom`, `hor`, `ver`, `all`

`scrollbar`: `auto`, `off`, `on`, `active`

`snap_x/snap_y`: `none`, `start`, `end`, `center`

## Type-Specific Methods

Basic methods:

- `label/button/checkbox/dropdown/textarea/list_text/list_button:set_text(text)`
- `bar/slider/roller:set_value(v[, anim])`
- `arc/scale/dropdown/checkbox/switch/spinbox:set_value(v)`
- `bar/slider/arc/scale/dropdown/roller/checkbox/switch/spinbox:get_value()`
- `bar/slider/arc/scale/spinbox:set_range(min, max)`
- `screen:load()`
- `image:set_raw_rgb565(width, height, data[, opts])` -> true
- `image:set_frame(frame)` -> true (zero-copy handoff from an `image.frame`)
- `image:set_raw_argb8888(width, height, data)` -> true
- `image:set_pivot(x, y)` -> true
- `image:set_rotation(angle)` -> true
- `line:set_points({{x=0,y=0}, {x=20,y=20}, ...})` -> true
- `list:add_text(text)` -> `list_text`
- `list:add_button(text[, symbol])` -> `list_button`
- `table:set_cell(row, col, text)`
- `table:get_cell(row, col)` -> string
- `buttonmatrix:set_map(map)`
- `buttonmatrix:set_selected(index)`
- `buttonmatrix:get_selected()` -> index or nil
- `buttonmatrix:get_button_text(index)` -> string
- `buttonmatrix:set_one_checked(bool)`
- `calendar:set_today(y, m, d)`
- `calendar:set_shown(y, m)`
- `calendar:set_highlighted({{y,m,d}, ...})`
- `calendar:get_pressed_date()` -> `{ year = y, month = m, day = d }` or nil
- `canvas:fill_bg(color[, opa])`
- `canvas:set_px(x, y, color[, opa])`
- `canvas:set_rgb565_data(data[, byte_order])`
- `canvas:get_px(x, y)` -> `{r, g, b, a}`
- `chart:add_series(color[, axis])` -> series handle
- `chart:set_type(type)`
- `chart:set_point_count(n)`
- `chart:set_range(min, max[, axis])`
- `chart:set_next_value(series, value)`
- `chart:set_series_values(series, values)`
- `chart:refresh()`
- `imagebutton:set_src(state, mid[, left, right])`
- `imagebutton:set_state(state)`
- `led:set_color(color)`
- `led:set_brightness(v)`
- `led:get_brightness()` -> integer
- `led:on()`, `led:off()`, `led:toggle()`
- `menu:page([title])` -> page
- `menu:cont(parent)` -> cont
- `menu:section(page)` -> section
- `menu:separator(page)` -> separator
- `menu:set_page([page])`
- `menu:set_sidebar_page([page])`
- `menu:set_mode_header(mode)`
- `menu:set_root_back_button(bool)`
- `menu:clear_history()`
- `msgbox:add_title(text)` -> msgbox_child
- `msgbox:add_text(text)` -> msgbox_child
- `msgbox:add_footer_button(text)` -> msgbox_child
- `msgbox:add_close_button()` -> msgbox_child
- `msgbox:close()`
- `msgbox:close_async()`
- `spangroup:add_span(text[, style])` -> span handle
- `spangroup:get_span_count()` -> integer
- `spangroup:refresh()`
- `span:set_text(text)`
- `span:get_text()` -> string
- `span:set_style(opts)`
- `span:delete()`
- `spinbox:set_step(v)`
- `spinbox:get_step()` -> integer
- `spinbox:increment()`
- `spinbox:decrement()`
- `spinbox:step_next()`
- `spinbox:step_prev()`
- `tabview:add_tab(name)` -> tab page
- `tabview:set_active(index[, anim])`
- `tabview:get_active()` -> index
- `tabview:get_tab_count()` -> integer
- `tabview:set_tab_text(index, text)`
- `tileview:add_tile(col, row[, dir])` -> tile
- `tileview:set_tile(tile[, anim])`
- `tileview:set_tile_by_index(col, row[, anim])`
- `tileview:get_active_tile()` -> tile or nil
- `lottie:set_src(path)` -> boolean
- `lottie:set_src_data(data)` -> boolean
- `lottie:restart()`, `lottie:pause()`, `lottie:resume()`
- `lottie:is_loaded()`, `lottie:is_playing()`
- `lottie:get_duration()`, `lottie:get_total_frames()`, `lottie:get_time()`
- `animimg:set_src(frames)`, `animimg:start()`, `animimg:stop()`
- `animimg:set_duration(ms)`, `animimg:get_duration()`
- `animimg:set_repeat_count(n)`, `animimg:get_repeat_count()`, `animimg:get_src_count()`
- `gif:set_src(path)`, `gif:restart()`, `gif:pause()`, `gif:resume()`
- `gif:is_loaded()`, `gif:get_size()`
- `gif:set_loop_count(n)`, `gif:get_loop_count()`, `gif:get_frame_count()`, `gif:get_current_frame()`
- `gif:set_auto_pause(bool)`
- `window:add_title(text)` -> label
- `window:add_button([icon[, width]])` -> button
- `window:get_header()` -> object
- `window:get_content()` -> object

`span:set_style(opts)` applies style options through the owning `spangroup`.

- `canvas.color_format`: `rgb565`, `rgb888`, `xrgb8888`, `argb8888`, `native`
- `chart.type`: `none`, `line`, `curve`, `bar`, `stacked`, `scatter`
- `chart.update_mode`: `shift`, `circular`
- `chart` axis: `primary_y`, `y`, `secondary_y`, `primary_x`, `x`, `secondary_x`
- `imagebutton` state: `released`, `pressed`, `disabled`,
  `checked_released`, `checked_pressed`, `checked_disabled`
- `spangroup.mode`: `fixed`, `expand`, `break`
- `spangroup.overflow`: `clip`, `ellipsis`
- `menu` header mode: `top_fixed`, `top_unfixed`, `bottom_fixed`
- Direction values: `none`, `left`, `right`, `top`, `bottom`, `hor`, `ver`, `all`

## Demos

- `lvgl.demos()` -> `{name, ...}`
- `lvgl.demo(name)` -> `true`

`lvgl.demo(name)` requires an initialized LVGL runtime. Available demo names depend on firmware LVGL demo build flags.

## Raw Images And Dynamic Lines

`image:set_frame(frame)` is a product extension: it takes an `image.frame`
from the `image` module, converts it to RGB565 once and points the widget at
that buffer without copying. The image store stays referenced until the object
is replaced or deleted. The widget size should match the frame size; use
`frame:info()` to read it.
Raw image setters accept tightly packed pixel strings. RGB565 uses little-endian bytes by default; `{swapped=true}` accepts big-endian RGB565. ARGB8888 uses B, G, R, A byte order. The string length must equal `width * height * bytes_per_pixel`. Dimensions must be positive integers; height is limited to 65535 pixels, and maximum width depends on pixel format and firmware. Oversized images raise an error.

Uploads replace the previous source. Argument, allocation, or unsupported-format errors leave the previous image intact. Lua strings and image frames may be released immediately after upload.

`set_rotation(angle)` accepts an integer in tenths of a degree, clockwise, normalized modulo 3600. `set_pivot(x, y)` uses pixel coordinates. Pivot and line coordinates must be integers in `[-32768, 32767]`. Image rotation requires firmware draw support for its color format.

Use `image` to load and convert a JPEG before displaying it:

```lua
local image = require("image")
local source <close> = image.load_file(path)
local cover <close> = image.resize(source, { width = 148, height = 148, format = image.RGB565 })
local picture = lvgl.image(scr, { w = 148, h = 148 })
picture:set_raw_rgb565(148, 148, cover:data())
picture:set_pivot(74, 74)
picture:set_rotation(450)
```

Line construction and `set_points()` accept 2–1024 point tables; omitted coordinates default to zero. Invalid arguments leave the previous line unchanged and raise a Lua error. Calls on deleted objects also raise an error.

## Limitations

- Encoder and keypad input are not available.
- Some widgets and demos depend on LVGL `LV_USE_*` build flags; disabled entries raise a Lua error such as `not enabled in firmware`.
- `lvgl.font_load(...)` requires LVGL `tiny_ttf` support.
- LVGL file sources use `D:` (Claw data root, `/fatfs/claw/data`) and, when a card is mounted, `S:` (SD mount point, e.g. `/sdcard`). PNG, JPEG and GIF decoders are enabled; other filesystems are not exposed to LVGL.
- Constructor `src` and `imagebutton:set_src(...)` accept `D:/...` or `S:/...` LVGL FS paths; PNG/JPEG are decoded by the firmware. Raw pixel decoding still goes through the `image` module.
- Canvas supports background fill, pixel read/write, and RGB565 pixel uploads. Advanced drawing layers are not available.
- Chart cursors and other advanced chart APIs are not wrapped.
- Span and chart series handles support only their documented methods, not general widget methods such as `set_pos`.
- Non-ASCII text rendering depends on either firmware-enabled fonts or a
  runtime TTF font applied with `font`.
