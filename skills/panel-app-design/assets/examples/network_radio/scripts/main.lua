-- 网络电台 / NEON RADIO for the 1024x600 ESP32-P4 panel.
--
-- A cyberpunk tuner: a static neon scene, a live spectrum driven by the MP3
-- decoder's RMS level, four SomaFM streams, and big touch targets. Radio uses
-- the native-rate MP3 exclusive path (the assistant audio is paused while it
-- plays and restored when the App exits).
local lvgl = require("lvgl")
local system = require("system")
local delay = require("delay")
local audio = require("audio")
local image = require("image")

lvgl.init({ buffer_lines = 40, tick_ms = 5, task_period_ms = 10 })

local app_src = debug.getinfo(1, "S").source or ""
if app_src:sub(1, 1) == "@" then
  app_src = app_src:sub(2)
end
local app_dir = app_src:match("^(.*)/scripts/[^/]+$") or
                "/fatfs/claw/data/apps/network_radio"

local W, H = 1024, 600
local VIZ_Y, VIZ_H = 300, 170
local VIZ_W = W

local function I(v) return math.floor(v + 0.5) end

local function hexc(n) return string.format("#%06x", n) end

local function hexmix(a, b, k)
  if k < 0 then k = 0 elseif k > 1 then k = 1 end
  local ar, ag, ab = (a >> 16) & 255, (a >> 8) & 255, a & 255
  local br, bg, bb = (b >> 16) & 255, (b >> 8) & 255, b & 255
  return (I(ar + (br - ar) * k) << 16) |
         (I(ag + (bg - ag) * k) << 8) |
         I(ab + (bb - ab) * k)
end

local STATIONS = {
  {
    name = "亚洲音乐台",
    tag = "ASIA FM · 华语流行",
    url = "https://lhttp.qtfm.cn/live/15318341/64k.mp3",
    kbps = 64,
    accent = 0x00e5ff,
  },
  {
    name = "轻松调频",
    tag = "HIT FM · 流行音乐",
    url = "https://lhttp.qtfm.cn/live/15318703/64k.mp3",
    kbps = 64,
    accent = 0xff2bd6,
  },
  {
    name = "中国之声",
    tag = "CNR-1 · 新闻与谈话",
    url = "https://lhttp.qtfm.cn/live/15318317/64k.mp3",
    kbps = 64,
    accent = 0xffcf6b,
  },
  {
    name = "亚洲热歌",
    tag = "ASIA FM · 热歌 192K",
    url = "http://hot.asiafm.net:8000/asiafm",
    kbps = 192,
    accent = 0x7dffcf,
  },
}

local C = {
  bg = 0x05070f,
  text = 0xf4f8ff,
  muted = 0x8ea6c8,
  dim = 0x607792,
  cyan = 0x00e5ff,
  magenta = 0xff2bd6,
  amber = 0xffcf6b,
  red = 0xff4d6d,
  green = 0x7dffcf,
}

local function load_font(px, cache)
  local ok, font = pcall(lvgl.font_load, app_dir .. "/assets/ui_cn.ttf",
                         { size = px, cache_size = cache })
  if ok and font then return font end
  return nil
end

local f_brand = load_font(18, 128)
local f_clock = load_font(24, 64)
local f_station = load_font(52, 96)
local f_tag = load_font(18, 160)
local f_big = load_font(48, 64)
local f_body = load_font(20, 160)
local f_small = load_font(14, 200)
local f_card = load_font(18, 160)
local f_card_tag = load_font(12, 200)
if f_body then lvgl.set_default_font(f_body) end

-- ---------------------------------------------------------------- background
local function image_new(w, h)
  local ok, cv = pcall(image.new, w, h, image.RGB565)
  if not ok or not cv then
    error("image.new failed: " .. tostring(cv))
  end
  return cv
end

local bg = image_new(W, H)

local function ring(cv, cx, cy, r, color, width, segments)
  local px, py = cx + r, cy
  for i = 1, segments do
    local a = i * 2 * math.pi / segments
    local x = cx + I(math.cos(a) * r)
    local y = cy + I(math.sin(a) * r)
    cv:line(px, py, x, y, color, width)
    px, py = x, y
  end
end

local function paint_background(cv)
  for i = 0, 59 do
    local k = i / 59
    local col
    if k < 0.5 then
      col = hexmix(0x070b18, 0x0c1d3a, k / 0.5)
    else
      col = hexmix(0x0c1d3a, 0x05070f, (k - 0.5) / 0.5)
    end
    cv:fill_rect(0, i * 10, W, 10, col)
  end

  -- horizon grid
  for x = -512, 1536, 96 do
    cv:line(512, 360, x, H - 1, 0x0b2238, 1)
  end
  for y = 380, 560, 24 do
    cv:fill_rect(0, y, W, 1, 0x0a1c2e)
  end
  cv:fill_rect(0, 360, W, 2, 0x123c58)

  -- tuner rings behind the right-hand readout
  ring(cv, 790, 142, 118, 0x0d3d55, 2, 120)
  ring(cv, 790, 142, 146, 0x2a1040, 2, 120)
  ring(cv, 790, 142, 176, 0x0d3d55, 1, 120)
  for i = 0, 59 do
    local a = i * 2 * math.pi / 60
    local x0 = 790 + I(math.cos(a) * 182)
    local y0 = 142 + I(math.sin(a) * 182)
    local x1 = 790 + I(math.cos(a) * 190)
    local y1 = 142 + I(math.sin(a) * 190)
    cv:line(x0, y0, x1, y1, i % 5 == 0 and 0x1f5a78 or 0x0b2238, 1)
  end

  -- stars
  local seed = 20261029
  for _ = 1, 90 do
    seed = (seed * 1103515245 + 12345) % 2147483648
    local x = seed % W
    seed = (seed * 1103515245 + 12345) % 2147483648
    local y = 40 + seed % 330
    local c = (seed % 7 == 0) and 0x9fdcff or 0x1b3450
    cv:fill_rect(x, y, 2, 2, c)
  end

  -- diagonal neon streaks
  cv:line(0, 560, 300, 360, 0x0d2a3d, 1)
  cv:line(724, 360, 1024, 560, 0x2a1030, 1)
  cv:fill_rect(0, 0, W, 2, 0x1b4a66)
end

-- ---------------------------------------------------------------- visualizer
local viz = image_new(VIZ_W, VIZ_H)
local BARS = 56
local BAR_STEP = (VIZ_W - 32) // BARS
local BAR_W = 12
local BASE = VIZ_H - 34
local peaks = {}
for i = 1, BARS do peaks[i] = 4 end

local function draw_viz(level, now, obj)
  viz:fill(0x05070f)
  viz:fill_rect(0, 0, VIZ_W, 1, 0x0a1c2e)
  viz:fill_rect(0, BASE + 1, VIZ_W, 1, 0x123c58)
  viz:fill_rect(0, BASE + 2, VIZ_W, VIZ_H - BASE - 2, 0x060a13)

  local gain = math.min(1, (level or 0) * 3.6)
  for i = 1, BARS do
    local x = 16 + (i - 1) * BAR_STEP
    local phase = now / 260 + i * 0.61
    local wave = 0.5 + 0.5 * math.sin(phase)
    local h = 4 + I((6 + gain * 118) * (0.35 + 0.65 * wave))
    if h < 4 then h = 4 end
    if h > BASE - 6 then h = BASE - 6 end
    peaks[i] = math.max(h, peaks[i] - 3)

    local col = hexmix(C.cyan, C.magenta, (i - 1) / (BARS - 1))
    viz:fill_rect(x, BASE - h, BAR_W, h, col)
    viz:fill_rect(x, BASE + 4, BAR_W, h // 4, hexmix(col, 0x05070f, 0.55))
    viz:fill_rect(x, BASE - peaks[i] - 3, BAR_W, 2, C.text)
  end

  local scan = 8 + (now // 55) % (VIZ_H - 16)
  viz:fill_rect(0, scan, VIZ_W, 1, 0x0d3248)
  obj:set_frame(viz)
end

-- ---------------------------------------------------------------- screen
local scr = lvgl.create_screen()
scr:set_style({ bg_color = hexc(C.bg), pad = 0 })
scr:clear_flag("SCROLLABLE")

paint_background(bg)
local bg_obj = lvgl.image(scr, { x = 0, y = 0, w = W, h = H, pad = 0 })
bg_obj:clear_flag("CLICKABLE")
bg_obj:set_frame(bg)

local viz_obj = lvgl.image(scr, { x = 0, y = VIZ_Y, w = VIZ_W, h = VIZ_H, pad = 0 })
viz_obj:clear_flag("CLICKABLE")
draw_viz(0, 0, viz_obj)

-- Header ----------------------------------------------------------
lvgl.label(scr, { text = "P4 // 霓虹电台", x = 32, y = 18,
                  text_color = hexc(C.cyan), font = f_brand })
local clock_lbl = lvgl.label(scr, { text = "--:--", align = "top_right", x = -32, y = 14,
                                   text_color = hexc(C.muted), font = f_clock })
local mode_lbl = lvgl.label(scr, { text = "网络电台 · 霓虹波段", x = 32, y = 44,
                                   text_color = hexc(C.dim), font = f_small })

-- Station hero ----------------------------------------------------
local station_shadow = lvgl.label(scr, { text = "", x = 60, y = 100,
                                         text_color = "#1a0b2e", font = f_station })
local station_lbl = lvgl.label(scr, { text = "", x = 58, y = 98,
                                      text_color = hexc(C.text), font = f_station })
local station_tag = lvgl.label(scr, { text = "", x = 62, y = 164,
                                      text_color = hexc(C.muted), font = f_tag })

-- ON AIR badge ----------------------------------------------------
local onair_box = lvgl.container(scr, {
  x = 60, y = 202, w = 166, h = 38,
  bg_color = "#160714", bg_opa = 220, radius = 19,
  border_color = hexc(C.magenta), border_width = 2, pad = 0,
})
onair_box:clear_flag("SCROLLABLE")
onair_box:clear_flag("CLICKABLE")
local onair_lbl = lvgl.label(onair_box, { text = "待机", align = "center", x = 0, y = 0,
                                          text_color = hexc(C.muted), font = f_body })

-- Right readout / controls ---------------------------------------
local index_lbl = lvgl.label(scr, { text = "01 / 04", align = "top_right", x = -32, y = 58,
                                     text_color = hexc(C.dim), font = f_small })
local kbps_lbl = lvgl.label(scr, { text = "128", x = 744, y = 92,
                                    text_color = hexc(C.cyan), font = f_big })
lvgl.label(scr, { text = "千比特/秒", x = 842, y = 120,
                  text_color = hexc(C.muted), font = f_small })
local format_lbl = lvgl.label(scr, { text = "-- 千赫 · 立体声", x = 744, y = 152,
                                     text_color = hexc(C.dim), font = f_small })

local play_btn = lvgl.container(scr, {
  x = 744, y = 196, w = 216, h = 54,
  bg_color = "#081826", bg_opa = 240, radius = 14,
  border_color = hexc(C.cyan), border_width = 2, pad = 0,
})
play_btn:clear_flag("SCROLLABLE")
play_btn:add_flag("CLICKABLE")
local play_lbl = lvgl.label(play_btn, { text = "播放", align = "center", x = 0, y = 0,
                                        text_color = hexc(C.cyan), font = f_body })

local minus_btn = lvgl.container(scr, {
  x = 744, y = 260, w = 44, h = 30,
  bg_color = "#081018", radius = 8, border_color = "#1a3a52", border_width = 1, pad = 0,
})
minus_btn:clear_flag("SCROLLABLE")
minus_btn:add_flag("CLICKABLE")
lvgl.label(minus_btn, { text = "-", align = "center", x = 0, y = -2,
                        text_color = hexc(C.muted), font = f_body })
local vol_lbl = lvgl.label(scr, { text = "音量 70", x = 796, y = 264,
                                  text_color = hexc(C.dim), font = f_small })
local plus_btn = lvgl.container(scr, {
  x = 916, y = 260, w = 44, h = 30,
  bg_color = "#081018", radius = 8, border_color = "#1a3a52", border_width = 1, pad = 0,
})
plus_btn:clear_flag("SCROLLABLE")
plus_btn:add_flag("CLICKABLE")
lvgl.label(plus_btn, { text = "+", align = "center", x = 0, y = -2,
                       text_color = hexc(C.muted), font = f_body })

-- Station cards ---------------------------------------------------
local select_station
local cards = {}
for i = 1, #STATIONS do
  local cx = 32 + (i - 1) * 240
  local box = lvgl.container(scr, {
    x = cx, y = 492, w = 228, h = 88,
    bg_color = "#070c14", bg_opa = 240, radius = 12,
    border_color = "#1a2c44", border_width = 1, pad = 0,
  })
  box:clear_flag("SCROLLABLE")
  box:add_flag("CLICKABLE")
  local idx = lvgl.label(box, { text = string.format("%02d", i), x = 14, y = 10,
                                 text_color = hexc(STATIONS[i].accent), font = f_small })
  local name = lvgl.label(box, { text = STATIONS[i].name, x = 14, y = 28,
                                  text_color = hexc(C.muted), font = f_card })
  local tag = lvgl.label(box, { text = STATIONS[i].tag, x = 14, y = 56,
                                text_color = hexc(C.dim), font = f_card_tag })
  cards[i] = { box = box, name = name, tag = tag, idx = idx }
  local index = i
  box:on("clicked", function() select_station(index, true) end)
end

-- ---------------------------------------------------------------- audio
local volume = 70
local ok_out, out = pcall(audio.open_output, { volume = volume })
if not ok_out then out = nil end
local player = nil
if out then
  local ok_p, p = pcall(audio.player, { output = out })
  if ok_p then player = p end
end
local current = 1
local last_error = nil

local function start_station()
  if not player then
    last_error = "AUDIO OFFLINE"
    return false
  end
  local ok, err = player:play(STATIONS[current].url, { wait = false })
  if not ok then
    last_error = tostring(err)
    return false
  end
  last_error = nil
  return true
end

select_station = function(index, autoplay)
  if index < 1 or index > #STATIONS then return end
  current = index
  local st = STATIONS[current]
  station_lbl:set_text(st.name)
  station_shadow:set_text(st.name)
  station_tag:set_text(st.tag)
  index_lbl:set_text(string.format("%02d / %02d", current, #STATIONS))
  kbps_lbl:set_text(tostring(st.kbps))
  kbps_lbl:set_style({ text_color = hexc(st.accent) })
  for i = 1, #cards do
    local sel = (i == current)
    local bg_color = "#070c14"
    local border_color = "#1a2c44"
    local border_width = 1
    local name_color = hexc(C.muted)
    if sel then
      bg_color = "#0b1d2e"
      border_color = hexc(STATIONS[i].accent)
      border_width = 2
      name_color = hexc(C.text)
    end
    cards[i].box:set_style({
      bg_color = bg_color,
      border_color = border_color,
      border_width = border_width,
    })
    cards[i].name:set_style({ text_color = name_color })
  end
  if autoplay then
    start_station()
  end
end

local function set_volume(v)
  if v < 0 then v = 0 elseif v > 100 then v = 100 end
  volume = v
  vol_lbl:set_text("音量 " .. volume)
  if out then pcall(out.set_volume, out, volume) end
end

play_btn:on("clicked", function()
  if not player then
    start_station()
    return
  end
  local ev = player:poll()
  if ev.state == "playing" or ev.state == "buffering" then
    player:stop()
  else
    start_station()
  end
end)
minus_btn:on("clicked", function() set_volume(volume - 10) end)
plus_btn:on("clicked", function() set_volume(volume + 10) end)

-- ---------------------------------------------------------------- status
local function update_ui(ev)
  local state = ev and ev.state or "idle"
  local color, label
  if state == "playing" then
    color, label = C.magenta, "直播中"
  elseif state == "buffering" then
    color, label = C.amber, "缓冲中"
  elseif state == "error" then
    color, label = C.red, "离线"
  elseif state == "stopped" then
    color, label = C.dim, "待机"
  else
    color, label = C.dim, "待机"
  end
  onair_lbl:set_text(label)
  onair_lbl:set_style({ text_color = hexc(color) })
  onair_box:set_style({ border_color = hexc(color) })

  if state == "playing" or state == "buffering" then
    play_lbl:set_text(state == "playing" and "停止" or "取消")
    play_lbl:set_style({ text_color = hexc(C.magenta) })
  else
    play_lbl:set_text("播放")
    play_lbl:set_style({ text_color = hexc(C.cyan) })
  end

  if state == "playing" then
    local info = ev.music_info or {}
    local rate = tonumber(info.source_sample_rate) or 0
    local channels = tonumber(info.source_channels) or 0
    if rate > 0 then
      format_lbl:set_text(string.format("%.1f 千赫 · %s", rate / 1000,
                                        channels == 2 and "立体声" or "单声道"))
    else
      format_lbl:set_text("-- 千赫 · 立体声")
    end
    station_tag:set_text(STATIONS[current].tag)
  elseif state == "buffering" then
    format_lbl:set_text("正在锁定信号…")
    station_tag:set_text("正在连接 " .. STATIONS[current].tag)
  elseif state == "error" then
    format_lbl:set_text("信号中断")
    station_tag:set_text((last_error or (ev and ev.error) or "流媒体错误"):sub(1, 48))
  else
    format_lbl:set_text("-- 千赫 · 立体声")
    station_tag:set_text("轻触播放开始收听")
  end
end

local function update_clock()
  local ok, t = pcall(os.date, "*t")
  if ok and type(t) == "table" and t.hour then
    clock_lbl:set_text(string.format("%02d:%02d", t.hour, t.min))
  end
end

-- ---------------------------------------------------------------- loop
scr:load()
update_clock()
select_station(1, true)
local last_viz, last_clock = 0, 0
draw_viz(0, 0, viz_obj)

while not claw.abort_requested() do
  local now = system.millis()
  local ev = player and player:poll() or nil
  update_ui(ev)
  if now - last_viz >= 100 then
    last_viz = now
    draw_viz(ev and ev.level or 0, now, viz_obj)
  end
  if now - last_clock >= 1000 then
    last_clock = now
    update_clock()
  end
  lvgl.process_events(20)
  delay.delay_ms(10)
end

if player then
  pcall(player.stop, player)
  pcall(player.close, player)
end
if out then pcall(out.close, out) end
