-- Requires media files under the Claw data root (LVGL drive D:):
--   D:/<media_dir>/f0.png f1.png f2.png
--   D:/<media_dir>/f0.jpg f1.jpg f2.jpg
--   D:/<media_dir>/anim.gif
-- Override the directory via test args: { media_dir = "media" }.
local delay = require("delay")
local lvgl = require("lvgl")
local storage = require("storage")

local function check(cond, msg)
    if not cond then
        error(msg or "check failed", 2)
    end
end

local function check_eq(actual, expected, msg)
    if actual ~= expected then
        error((msg or "check_eq failed") .. ": expected " .. tostring(expected) .. ", got " .. tostring(actual), 2)
    end
end

local function data_path_for(rel_path)
    return storage.join_path(storage.get_root_dir(), rel_path)
end

local function lvgl_fs_path_for(rel_path)
    return "D:/" .. rel_path
end

local test_args = args or {}
local media_dir = test_args.media_dir or "media"
local png_frames = { "f0.png", "f1.png", "f2.png" }
local jpg_frames = { "f0.jpg", "f1.jpg", "f2.jpg" }
local gif_file = "anim.gif"

local function require_media(rel_path)
    local data_path = data_path_for(media_dir .. "/" .. rel_path)
    if not storage.exists(data_path) then
        error("missing test media: " .. data_path .. " (or pass args.media_dir)")
    end
end

for _, name in ipairs(png_frames) do require_media(name) end
for _, name in ipairs(jpg_frames) do require_media(name) end
require_media(gif_file)

lvgl.init({
    buffer_lines = 10,
    tick_ms = 5,
    task_period_ms = 10,
})

local ok, err = pcall(function()
    local scr = lvgl.create_screen()
    scr:set_style({ bg_color = "#101820" })

    local png_src = {}
    local jpg_src = {}
    for _, name in ipairs(png_frames) do
        png_src[#png_src + 1] = lvgl_fs_path_for(media_dir .. "/" .. name)
    end
    for _, name in ipairs(jpg_frames) do
        jpg_src[#jpg_src + 1] = lvgl_fs_path_for(media_dir .. "/" .. name)
    end
    local gif_src = lvgl_fs_path_for(media_dir .. "/" .. gif_file)

    local png_anim = lvgl.animimg(scr, {
        src = png_src,
        duration = 900,
        repeat_count = -1,
        align = "top_mid",
        y = 16,
    })
    check_eq(png_anim:get_src_count(), #png_src, "PNG animimg src count")
    check_eq(png_anim:get_duration(), 900, "PNG animimg duration")
    check_eq(png_anim:get_repeat_count(), -1, "PNG animimg repeat count")

    local jpg_anim = lvgl.animimg(scr, {
        src = jpg_src,
        duration = 600,
        repeat_count = 0,
        align = "top_mid",
        y = 96,
    })
    check_eq(jpg_anim:get_src_count(), #jpg_src, "JPEG animimg src count")
    check_eq(jpg_anim:get_repeat_count(), 0, "JPEG animimg repeat count")

    local gif = lvgl.gif(scr, {
        src = gif_src,
        align = "bottom_mid",
        y = -16,
        color_format = "rgb565",
        auto_pause = true,
    })
    check(gif:is_loaded(), "GIF should load from " .. gif_src)
    local gif_w, gif_h = gif:get_size()
    check(gif_w > 0 and gif_h > 0, "GIF should report its size")
    check(gif:get_frame_count() > 0, "GIF should report frame count")
    check(gif:set_loop_count(2), "GIF set loop count")
    gif:pause()
    gif:resume()
    gif:restart()

    local img_png = lvgl.image(scr, { src = png_src[1], align = "top_left", x = 8, y = 8 })
    local img_jpg = lvgl.image(scr, { src = jpg_src[1], align = "top_left", x = 76, y = 8 })

    scr:load()
    delay.delay_ms(200)
    check(img_png:get_property("w") > 0, "PNG decoder should size the image")
    check(img_jpg:get_property("w") > 0, "JPEG decoder should size the image")

    local frame = gif:get_current_frame()
    check(frame >= 0 and frame < gif:get_frame_count(), "GIF frame index")

    delay.delay_ms(2000)
end)

lvgl.deinit()

if not ok then
    error(err)
end
