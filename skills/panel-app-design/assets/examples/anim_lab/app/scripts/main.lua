-- anim_lab: small LVGL animation demo for the 1024x600 panel.
-- Lottie loader (ThorVG) + animimg frame sequence, both with a package
-- fallback when the SD upgrade is missing.
local lvgl = require("lvgl")
local system = require("system")
local delay = require("delay")

lvgl.init({ buffer_lines = 40, tick_ms = 5, task_period_ms = 10 })

local src = debug.getinfo(1, "S").source or ""
if src:sub(1, 1) == "@" then src = src:sub(2) end
local app_dir = src:match("^(.*)/scripts/[^/]+$") or "/fatfs/claw/data/apps/anim_lab"
local app_id = app_dir:match("([^/]+)$") or "anim_lab"

local DATA_ROOT = "/fatfs/claw/data"
local SD_ROOT = "/sdcard"

local function to_lvgl_fs(abs_dir)
  if abs_dir:sub(1, #SD_ROOT) == SD_ROOT then
    return "S:" .. abs_dir:sub(#SD_ROOT + 1)
  end
  if abs_dir:sub(1, #DATA_ROOT) == DATA_ROOT then
    return "D:" .. abs_dir:sub(#DATA_ROOT + 1)
  end
  return nil
end

local function exists(path)
  local f = io.open(path, "rb")
  if not f then return false end
  f:close()
  return true
end

local pkg_anim_abs = app_dir .. "/assets/anim"
local pkg_anim_fs = to_lvgl_fs(pkg_anim_abs)
local sd_anim_abs = SD_ROOT .. "/panel-assets/" .. app_id .. "/anim"
local sd_anim_fs = "S:/panel-assets/" .. app_id .. "/anim"

-- Pick the SD upgrade only when every frame is present; otherwise the package
-- frames are used and the App still animates without a card.
local SD_FRAMES = 12
local PKG_FRAMES = 6
local use_sd_frames = true
for i = 0, SD_FRAMES - 1 do
  if not exists(string.format("%s/f%d.png", sd_anim_abs, i)) then
    use_sd_frames = false
    break
  end
end

local frame_count = use_sd_frames and SD_FRAMES or PKG_FRAMES
local frame_base = use_sd_frames and sd_anim_fs or pkg_anim_fs
local frames = {}
for i = 0, frame_count - 1 do
  frames[#frames + 1] = string.format("%s/f%d.png", frame_base, i)
end

local use_sd_lottie = exists(sd_anim_abs .. "/loader.json")
local lottie_path = use_sd_lottie and (sd_anim_abs .. "/loader.json")
                        or (pkg_anim_abs .. "/loader.json")

local C = {
  bg = 0x0a0f14,
  panel = 0x0e151c,
  panel2 = 0x131d26,
  line = 0x1d2a35,
  text = 0xe8f0ea,
  muted = 0x8a9a96,
  dim = 0x4e5d5a,
  lime = 0xc8ff3e,
  cyan = 0x62d4ff,
}

local function hexc(n) return string.format("#%06x", n) end

local function load_font(px, cache)
  local ok, font = pcall(lvgl.font_load, app_dir .. "/assets/ui_cn.ttf",
                         { size = px, cache_size = cache })
  if ok and font then return font end
  return nil
end

local f_title = load_font(30, 96)
local f_name = load_font(20, 96)
local f_small = load_font(12, 160)
local f_btn = load_font(16, 96)

local function label(parent, text, x, y, color, font, w)
  local opts = { text = text, x = x, y = y, text_color = hexc(color) }
  if font then opts.font = font end
  if w then opts.w = w end
  local obj = lvgl.label(parent, opts)
  if obj then obj:clear_flag("SCROLLABLE") end
  return obj
end

local function card(parent, x)
  local obj = lvgl.container(parent, {
    x = x, y = 140, w = 420, h = 360,
    bg_color = hexc(C.panel), border_color = hexc(C.line),
    border_width = 1, radius = 0, pad = 0,
  })
  if obj then obj:clear_flag("SCROLLABLE") end
  return obj
end

local scr = lvgl.create_screen()
scr:set_style({ bg_color = hexc(C.bg), pad = 0 })
scr:clear_flag("SCROLLABLE")

label(scr, "动效实验", 48, 28, C.text, f_title)
label(scr, "LOTTIE / ANIMIMG", 48, 76, C.dim, f_small)
lvgl.label(scr, {
  text = "素材来源：" .. (use_sd_frames and "SD" or "包内"),
  align = "top_right", x = -48, y = 46,
  text_color = hexc(use_sd_frames and C.cyan or C.muted), font = f_small,
})

lvgl.container(scr, {
  x = 48, y = 110, w = 928, h = 1,
  bg_color = hexc(C.line), border_width = 0, radius = 0, pad = 0,
})

-- Lottie card ---------------------------------------------------------
local card1 = card(scr, 72)
local ok_lottie, lottie = pcall(function()
  return lvgl.lottie(card1, {
    src = lottie_path, w = 160, h = 160, align = "top_mid", y = 30,
  })
end)
if not ok_lottie then lottie = nil end

label(card1, "LOTTIE 矢量循环", 28, 224, C.text, f_name)
label(card1, use_sd_lottie and "loader.json · SD 双轨" or "loader.json · 包内",
      28, 258, C.dim, f_small, 364)
local lottie_state = label(card1, ok_lottie and "运行中" or "加载失败",
                           28, 300, ok_lottie and C.lime or C.muted, f_small)

-- animimg card --------------------------------------------------------
local card2 = card(scr, 532)
local ok_anim, animimg = pcall(function()
  return lvgl.animimg(card2, {
    src = frames, duration = 1200, repeat_count = -1,
    align = "top_mid", y = 30,
  })
end)
if not ok_anim then animimg = nil end

label(card2, "ANIMIMG 序列帧", 28, 224, C.text, f_name)
label(card2, string.format("%d 帧 · 1200 ms · %s", frame_count,
                           use_sd_frames and "SD" or "包内"),
      28, 258, C.dim, f_small, 364)
local anim_state = label(card2, ok_anim and "运行中" or "加载失败",
                         28, 300, ok_anim and C.lime or C.muted, f_small)

-- Footer --------------------------------------------------------------
label(scr, "无卡或素材缺失时自动使用包内动画", 48, 544, C.dim, f_small)

local playing = true
local pause_btn = lvgl.button(scr, {
  text = "暂停", x = 828, y = 528, w = 148, h = 48,
  bg_color = hexc(C.panel2), text_color = hexc(C.lime),
  border_color = hexc(C.lime), border_width = 1, radius = 0, font = f_btn,
})
pause_btn:on("clicked", function()
  playing = not playing
  if lottie then
    if playing then lottie:resume() else lottie:pause() end
  end
  if animimg then
    if playing then animimg:start() else animimg:stop() end
  end
  pause_btn:set_text(playing and "暂停" or "继续")
  lottie_state:set_text(playing and "运行中" or "已暂停")
  anim_state:set_text(playing and "运行中" or "已暂停")
end)

scr:load()

print(string.format("[anim_lab] lottie=%s/%d frames, animimg=%s/%d frames, sd_lottie=%s sd_frames=%s",
  tostring(ok_lottie), ok_lottie and lottie:get_total_frames() or -1,
  tostring(ok_anim), ok_anim and animimg:get_src_count() or -1,
  tostring(use_sd_lottie), tostring(use_sd_frames)))

while not claw.abort_requested() do
  lvgl.process_events(20)
  delay.delay_ms(8)
end

lvgl.deinit()
