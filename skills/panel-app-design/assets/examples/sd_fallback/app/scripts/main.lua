-- Poster-style demo: FAT package always works, SD assets are an optional upgrade.
-- L0: full-screen JPEG, L1: transparent sigil sprite, L2: LVGL text/buttons.
-- SD assets live under /sdcard/panel-assets/<app_id>/ and are never required.

local lvgl = require("lvgl")
local system = require("system")

local app_src = debug.getinfo(1, "S").source or ""
if app_src:sub(1, 1) == "@" then
  app_src = app_src:sub(2)
end
local app_dir = app_src:match("^(.*)/scripts/[^/]+$") or
                "/fatfs/claw/data/apps/sd_fallback"
local app_id = app_dir:match("([^/]+)$") or "sd_fallback"
local SD_ROOT = "/sdcard/panel-assets/" .. app_id .. "/"

local ok_image, image = pcall(require, "image")
local ok_audio, audio = pcall(require, "audio")

local echoes = 0
local echo_lbl

local C = {
  bg       = "#070a12",
  panel    = "#0b111a",
  gold     = "#e8be69",
  gold_dim = "#8a6a36",
  text     = "#f4ead6",
  muted    = "#a9997c",
  ember    = "#ff9c40",
}

lvgl.init({ buffer_lines = 40, tick_ms = 5, task_period_ms = 10 })

local function load_font(px, cache)
  local ok, font = pcall(lvgl.font_load,
                         app_dir .. "/assets/ui_cn.ttf",
                         { size = px, cache_size = cache })
  if ok and font then
    return font
  end
  return nil
end

local f_small = load_font(16, 64)
local f_ui = load_font(20, 128)
local f_title = load_font(48, 64)
if f_ui then
  lvgl.set_default_font(f_ui)
end

local scr = lvgl.create_screen()
scr:set_style({ bg_color = C.bg, pad = 0 })
scr:clear_flag("SCROLLABLE")

local function play_click()
  if ok_audio and audio and audio.play then
    pcall(audio.play, "click")
  end
end

local function add_rgb565_image(parent, path, x, y, w, h)
  if not ok_image or not image then
    return nil
  end
  local ok, frame = pcall(image.load_file, path)
  if not ok or not frame then
    return nil
  end
  local ok2, rgb = pcall(image.convert, frame, image.RGB565)
  pcall(frame.release, frame)
  if not ok2 or not rgb then
    return nil
  end
  local ok3, data = pcall(rgb.data, rgb)
  pcall(rgb.release, rgb)
  if not ok3 or not data then
    return nil
  end

  local obj = lvgl.image(parent, { x = x, y = y, w = w, h = h, pad = 0 })
  local ok4 = pcall(obj.set_raw_rgb565, obj, w, h, data)
  if not ok4 then
    obj:add_flag("HIDDEN")
    return nil
  end
  return obj
end

local function add_argb_image(parent, path, x, y, w, h)
  local file = io.open(path, "rb")
  if not file then
    return nil
  end
  local data = file:read("*a")
  file:close()
  if not data or #data ~= w * h * 4 then
    return nil
  end

  local obj = lvgl.image(parent, { x = x, y = y, w = w, h = h, pad = 0 })
  local ok = pcall(obj.set_raw_argb8888, obj, w, h, data)
  if not ok then
    obj:add_flag("HIDDEN")
    return nil
  end
  return obj
end

-- L0: background, SD first, then the package fallback.
local bg_src = "none"
local bg = add_rgb565_image(scr, SD_ROOT .. "img/bg.jpg", 0, 0, 1024, 600)
if bg then
  bg_src = "sd"
else
  bg = add_rgb565_image(scr, app_dir .. "/assets/img/bg_fallback.jpg", 0, 0, 1024, 600)
  if bg then
    bg_src = "fat"
  else
    lvgl.container(scr, {
      x = 0, y = 0, w = 1024, h = 600, bg_color = C.bg,
      border_width = 0, pad = 0,
    }):clear_flag("CLICKABLE")
    bg_src = "programmatic"
  end
end
if bg then
  bg:clear_flag("CLICKABLE")
end

-- L1: transparent sigil sprite. ARGB from SD, opaque JPEG fallback in the package.
local sig_src = "none"
local sigil = add_argb_image(scr, SD_ROOT .. "img/sigil.argb", 422, 142, 180, 180)
if sigil then
  sig_src = "sd"
else
  sigil = add_rgb565_image(scr, app_dir .. "/assets/img/sigil_fallback.jpg", 422, 142, 180, 180)
  if sigil then
    sig_src = "fat"
  end
end
if sigil then
  sigil:clear_flag("CLICKABLE")
end

-- Background click target, created before the panel so the button stays on top.
local hit = lvgl.container(scr, {
  x = 0, y = 0, w = 1024, h = 600,
  bg_opa = 0, border_width = 0, pad = 0,
})
hit:clear_flag("SCROLLABLE")
hit:add_flag("CLICKABLE")
hit:on("clicked", function()
  echoes = echoes + 1
  echo_lbl:set_text(string.format("回响 · %02d", echoes))
  play_click()
end)

-- L2: typography and controls. Text stays in the FAT package.
lvgl.label(scr, {
  text = "余烬回廊", align = "top_mid", y = 30,
  text_color = C.gold, font = f_title,
})
lvgl.label(scr, {
  text = "EMBER GALLERY", align = "top_mid", y = 104,
  text_color = C.muted, font = f_small,
})

local panel = lvgl.container(scr, {
  x = 202, y = 344, w = 620, h = 220,
  bg_color = C.panel, bg_opa = 205, radius = 18,
  border_color = C.gold_dim, border_width = 1, pad = 0,
})
panel:clear_flag("SCROLLABLE")
panel:clear_flag("CLICKABLE")

local status_lbl = lvgl.label(panel, {
  text = "内置后备", align = "top_mid", y = 16,
  text_color = C.ember, font = f_ui,
})
status_lbl:clear_flag("CLICKABLE")

local source_lbl = lvgl.label(panel, {
  text = "素材来源：内置后备", align = "top_mid", y = 52,
  text_color = C.text, font = f_ui,
})
source_lbl:clear_flag("CLICKABLE")

echo_lbl = lvgl.label(panel, {
  text = "回响 · 00", align = "top_mid", y = 86,
  text_color = C.muted, font = f_small,
})
echo_lbl:clear_flag("CLICKABLE")

local btn = lvgl.button(panel, {
  text = "轻触继续", align = "bottom_mid", y = -18, w = 200, h = 48,
  bg_color = C.ember, text_color = C.bg, font = f_ui,
  border_color = C.gold_dim, border_width = 1, radius = 14, pad = 0,
})
btn:clear_flag("SCROLLABLE")

local sd_count = (bg_src == "sd" and 1 or 0) + (sig_src == "sd" and 1 or 0)
local fat_count = (bg_src == "fat" and 1 or 0) + (sig_src == "fat" and 1 or 0)
if sd_count == 2 then
  status_lbl:set_text("SD 高清")
  source_lbl:set_text("素材来源：外部存储")
elseif sd_count == 1 then
  status_lbl:set_text("混合素材")
  source_lbl:set_text("素材来源：外部存储 + 内置后备")
else
  status_lbl:set_text("内置后备")
  source_lbl:set_text("素材来源：内置后备")
end
print(string.format("sd_fallback bg=%s sigil=%s fat=%d sd=%d",
                    bg_src, sig_src, fat_count, sd_count))

btn:on("clicked", function()
  echoes = echoes + 1
  echo_lbl:set_text(string.format("回响 · %02d", echoes))
  play_click()
end)

scr:load()

while not claw.abort_requested() do
  lvgl.process_events(20)
end

-- The LVGL runtime owns fonts and widgets; let deinit release them together.
lvgl.deinit()
