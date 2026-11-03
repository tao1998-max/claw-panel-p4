# Lua Image

Shared image type, canvas and conversion helpers for Lua. Frames produced by
`image.load_file()` or `image.new()` are `image.frame` objects that can be
converted, resized, drawn into, saved, or handed to LVGL without copying
(`img:set_frame(frame)` in `lua_module_lvgl`).

This firmware build has no `camera`/`vision` modules; frames come from files or
canvases only.

## How to call

- `local image = require("image")`
- `image.convert(frame, format)` returns an `image.frame` in the requested
  `image.*` format.
- `image.resize(frame, opts)` returns a new, independent `image.frame` scaled
  to `opts.width` x `opts.height`. See "Example: resize" below.
- `image.crop(frame, x, y, w, h)`, `image.rotate(frame, 90|180|270)` and
  `image.flip(frame, "horizontal"|"vertical")` return transformed frames.
- `image.load_file(path [, opts])` reads an image file. With
  `opts.max_width` / `opts.max_height` the JPEG is decoded immediately at a
  reduced decoder stage (thumbnails).
- `image.load_raw(path, width, height [, format])` reads tightly packed
  RGB565/GRAY8 pixels (for backgrounds dithered offline).
- `image.encode(frame [, opts])` returns a JPEG frame using an explicit
  `opts.quality` / `opts.subsample`.
- `image.save_file(path, frame [, opts])` saves a frame using the format
  implied by the file suffix. With `opts` the frame is re-encoded at the
  requested JPEG quality.
- `image.new(width, height, format)` creates a writable canvas (`image.RGB565`
  or `image.GRAY8`). Canvas frames accept the drawing methods below.
- Invalid arguments and image I/O failures raise Lua errors.

## Canvas drawing (`image.new`)

```lua
local canvas <close> = image.new(400, 220, image.RGB565)
canvas:fill("#101826")                       -- whole frame
canvas:fill_rect(10, 10, 120, 80, 0x3366ff)  -- x, y, w, h, colour [, opacity 0-255]
canvas:line(10, 200, 390, 120, "#ffcc44", 3) -- x0, y0, x1, y1, colour [, width 1-16]
canvas:blit(icon_frame, 250, 100)            -- copy any frame (JPEG is decoded/converted)
canvas:blit(icon_frame, 250, 100, 128)       -- blend at 50 % opacity
image.save_file("/sdcard/apps/demo/canvas.jpg", canvas)
```

Colours are `0xRRGGBB` integers or `"#rrggbb"` strings. `blit()` converts the
source to the destination format automatically (including JPEG), clips to the
canvas and releases the temporary view. The methods are C loops over the
canvas buffer: 2000 4x4 alpha rectangles take about 40 ms on the P4, versus
hundreds of milliseconds of Lua string building.

Opaque fills (no opacity argument, or `opacity = 255`) paint one row and
`memcpy` it across the rectangle: a full 1024x600 `fill()` takes about 19 ms,
versus about 64 ms for per-pixel alpha blending. Use opaque fills for
backgrounds and reserve alpha for overlays.

Only frames created by `image.new()` are writable; loaded or converted frames
raise "image frame is read-only" when drawn into.

## Zero-copy display (LVGL)

```lua
local img = lvgl.image(scr, { x = 0, y = 0, w = 400, h = 220 })
img:set_frame(canvas)   -- no pixel copy; the image module keeps the buffer alive
```

`set_frame(frame)` converts the frame to RGB565 once, hands the buffer to LVGL
and keeps the image store referenced until the object is replaced or deleted.
In the reference firmware 100 calls take about 16 ms (0.16 ms each), compared
with copying a 1024x600 RGB565 buffer (1.2 MB) per frame with `frame:data()` +
`img:set_raw_rgb565()`.

## JPEG quality and subsampling

`image.encode()` and `image.save_file()` accept an options table:

```lua
local jpeg = image.encode(canvas, { quality = 85, subsample = "420" })
image.save_file("/sdcard/apps/demo/thumb.jpg", canvas, { quality = 60 })
```

- `quality` is `1..100` (default `80`), clamped.
- `subsample` is `"444"`, `"422"` (default) or `"420"`; `"gray"` forces a
  grayscale JPEG (only meaningful for `image.GRAY8` sources).
- `image.encode()` returns a JPEG `image.frame`; if the source is already a
  JPEG and no options are given it returns a view of the same buffer without
  re-encoding.
- `save_file(path, frame)` without options keeps the existing fast path: a
  frame that already carries JPEG data is written as-is; other formats are
  encoded with the defaults.
- Measurements on the P4 with a 640x480 canvas: q30/4:2:2 = 10.9 KB,
  q90/4:4:4 = 26.0 KB.

## Thumbnails and decode-time scaling

JPEG decoding can reduce the image by 1/2, 1/4 or 1/8 before it is written to
memory. This is much cheaper than decoding full size and resizing afterwards:

```lua
-- 640x480 JPEG -> 320x240 RGB565, no full-size RGB565 buffer at all
local thumb <close> = image.load_file(path, { max_width = 320, max_height = 240 })
-- info().pixel_format == "RGBP", info().width == 320
```

`max_width` / `max_height` are upper bounds. The decoder only supports stages
whose scaled dimensions are multiples of 8, so the call is best effort: when
no stage fits, the image is decoded at full size (check `frame:info()`). The
source file buffer is released immediately after decoding.

`image.resize()` uses the same path when the source frame has not been decoded
yet: `image.resize(jpeg_frame, { width = 160, height = 120 })` on a 640x480
JPEG decodes at 1/4 and returns directly. Resizing an already decoded RGB565
frame uses PPA scaling when `filter = "bilinear"`.

## Raw RGB565 backgrounds

The on-device JPEG decoder handles 4:2:0 chroma; very dark, smooth gradients can
show colour blocks. If that is actually visible on the panel, decode and dither
the image offline, store tightly packed RGB565 on SD, and display it without
any runtime JPEG decode (on the reference weather App the difference was not
visible, so it stays on JPEG):

```lua
local bg <close> = image.load_raw(app_dir .. "/assets/bg.rgb565", 1024, 600)
img:set_frame(bg)          -- zero copy, no Lua string, no resize
```

- The file size must be exactly `width * height * 2` for RGB565 (or `* 1` for
  GRAY8); any other size raises an error.
- `tools/make_raw_background.py` decodes a JPEG with ffmpeg and applies an
  ordered 8x8 Bayer dither before packing RGB565; this removes banding in
  smooth dark gradients and avoids the device decoder's chroma upsampling.
- Raw files are uncompressed: 1.2 MiB for 1024x600. Keep them on SD, not FAT.

## Crop, rotate and flip

```lua
local crop   <close> = image.crop(canvas, 10, 20, 100, 50) -- x, y, width, height
local turned <close> = image.rotate(canvas, 90)            -- clockwise: 90/180/270
local mirror <close> = image.flip(canvas, "horizontal")    -- or "vertical"
```

Each call returns a new independent frame; the source is not modified. Crop
coordinates are clamped to the source bounds. Output format is RGB565, or
GRAY8 when the source frame is GRAY8.

The transforms use PPA hardware when available and fall back to C loops for
RGB565/GRAY8. `image.rotate()` on a JPEG frame that has not been decoded yet
uses the decoder's rotation stage (clockwise), avoiding an intermediate
unrotated RGB565 buffer; it falls back to decode + PPA when the JPEG dimensions
are not multiples of 8.

## Format constants

Use these constants with `image.convert(frame, format)`:

| Constant | Output format |
|---|---|
| `image.RGB565` | RGB565 little-endian |
| `image.RGB565_BE` | RGB565 big-endian |
| `image.RGB888` | RGB888 |
| `image.BGR888` | BGR888 |
| `image.GRAY8` | 8-bit grayscale |
| `image.YUYV` | YUV 4:2:2 packed |
| `image.UYVY` | YUV 4:2:2 packed (swapped) |
| `image.JPEG` | JPEG still |
| `image.MJPEG` | Motion-JPEG frame |

## Frame type: `image.frame`

An `image.frame` is a Lua-visible image view. Methods:

- `frame:info()` returns `{ width, height, bytes, pixel_format, timestamp_us, valid }`
- `frame:data()` copies the buffer into a Lua string (slow, allocates)
- `frame:release()` releases this view; it can no longer be read or drawn

Release happens automatically when the variable is declared with the Lua 5.4+
`<close>` attribute or when it is collected by GC, but explicit `<close>` is the
recommended style:

```lua
do
    local frame <close> = image.load_file("/sdcard/panel-assets/demo.jpg")
    -- ... use frame ...
end
-- frame is already released here
```

Releasing the source frame does not invalidate converted views that are still
alive. Release every view of a source frame promptly so its memory is reclaimed.

## Pixel format names

`frame:info().pixel_format` is a 4-character FOURCC string. The image module
understands these tokens:

| Token | Meaning |
|---|---|
| `RGBP` | RGB565 little-endian |
| `RGBR` | RGB565 big-endian |
| `RGB3` | RGB888 |
| `BGR3` | BGR888 |
| `GREY` / `Y800` | 8-bit grayscale |
| `YUYV` | YUV 4:2:2 packed |
| `UYVY` | YUV 4:2:2 packed (swapped) |
| `JPEG` | JPEG still |
| `MJPG` | Motion-JPEG frame |

Consumers request the format they need; scripts usually pass the frame object directly.

## Example: convert a frame

```lua
local image = require("image")

do
    local gray <close> = image.convert(frame, image.GRAY8)
    local jpeg <close> = image.convert(frame, image.JPEG)
end
```

The converted result is a new `image.frame` view. Conversion results are cached
inside the frame, so converting the same frame to the same format again is free
(a borrowed view). Convert once, then resize/save/blit the converted frame.

## Example: resize

`image.resize(frame, opts)` returns a new, independent `image.frame` at
`opts.width` x `opts.height`. Optional `opts.format` selects the output
(`image.RGB565` or `image.GRAY8` only; defaults to RGB565, or GRAY8 when the
source is already gray). Optional `opts.filter` is `"nearest"` (default) or
`"bilinear"`. Output dimensions follow the same 1920 x 1080 pixel cap as
conversion. When the source is a JPEG that has not been decoded yet, the
decoder's 1/2..1/8 stages are used first, so large photos downscale cheaply
(640x480 -> 160x120 takes about 46 ms on the P4).

```lua
local image = require("image")
local storage = require("storage")

do
    local small <close> = image.resize(frame, { width = 96, height = 96 })

    local probe <close> = image.resize(frame, {
        width  = 64,
        height = 64,
        format = image.GRAY8,
        filter = "bilinear",
    })

    local thumb <close> = image.resize(frame, { width = 160, height = 120 })
    local jpeg  <close> = image.convert(thumb, image.JPEG)
    image.save_file(storage.join_path(storage.get_root_dir(), "thumb.jpg"), jpeg)
end
```

## Example: load JPEG from disk

```lua
local display = require("display")
local image   = require("image")
local storage = require("storage")
local screen <close> = display.open()
local info = screen:info()

do
    local frame <close> = image.load_file(storage.join_path(storage.get_root_dir(), "picture.jpg"))
    local rgb565 <close> = image.convert(frame, image.RGB565)
    screen:begin()
    screen:image(0, 0, rgb565, {
        mode = "contain",
        width = info.width,
        height = info.height,
    })
    screen:present()
    image.save_file(storage.join_path(storage.get_root_dir(), "copy.jpg"), frame)
end
```

`load_file()` and `save_file()` currently support `.jpg` / `.jpeg`. By default
the frame metadata reports the JPEG width, height, byte size, and
`pixel_format = "JPEG"`. With `max_width` / `max_height` the returned frame is
already decoded at a reduced stage (`pixel_format = "RGBP"`), which is the
cheapest way to build list or grid thumbnails from large photos.

## Resource limits

This module is designed for MCU-class devices and rejects oversized images
before conversion:

- JPEG files loaded from disk are limited to 4 MiB.
- Decoded or converted frames are limited to 1920 x 1080 pixels.
- Conversion and JPEG encoding may allocate additional buffers.

Use source image sizes and file sizes that fit the available memory budget, and
release all `image.frame` views promptly.

## Example: resize a photo and save it

```lua
local image          = require("image")
local storage        = require("storage")

do
    local frame <close> = image.load_file("/sdcard/panel-assets/photo.jpg")
    local small <close> = image.resize(frame,
        { width = 640, height = 480, filter = "bilinear" })
    image.save_file(storage.join_path(storage.get_root_dir(), "snapshot.jpg"), small)
end
```
