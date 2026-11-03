-- 天气 (weather_panel) for the 1024x600 ESP32-P4 panel.
--
-- Poster layout: a 1024x600 dusk illustration is decoded once into an RGB565
-- buffer and laid down as the bottom layer; the panels above it are translucent
-- containers so the artwork still reads through. set_frame() hands the decoded
-- buffer to LVGL without the old data()+set_raw_rgb565 copies.
--
-- Data: Open-Meteo (plain HTTP, no key, no redirect, ~2.2 KB). One request
-- returns current + 24 hourly points + 7 daily points + sunrise/sunset.
-- The payload is UTF-8 JSON but nothing decodes it: only numeric columns are
-- read, and every Chinese string lives in this file.
--
-- Two binding constraints drive the whole layout:
--   1. every coordinate must be an *integer* ("/" yields a float in Lua 5.4);
--   2. objects cannot be moved or resized after creation -- only text and
--      colours are settable.
--
-- (2) rules out any redrawn indicator, so weather icons are *glyphs*: ten
-- silhouettes injected into the bundled TTF at U+E000..U+E009 (see
-- tools/iconfont.py). An icon is therefore a label whose text is one character
-- -- it scales with font size, inherits text_color, and needs no bitmap.
--
-- Text is centred by giving every label a fixed width equal to its column, so
-- one- and two-digit values stay aligned without measuring strings.

local lvgl = require("lvgl")
local system = require("system")
local ok_net, net = pcall(require, "net")
local ok_img, image = pcall(require, "image")

local app_src = debug.getinfo(1, "S").source or ""
if app_src:sub(1, 1) == "@" then app_src = app_src:sub(2) end
local app_dir = app_src:match("^(.*)/scripts/[^/]+$") or
                "/fatfs/claw/data/apps/weather_panel"

-- ---------------------------------------------------------------- config
local CITY = { name = "郑州", lat = 34.75, lon = 113.62 }
local REFRESH_MS = 600000            -- weather moves slowly; 10 min is plenty

local W, H = 1024, 600
local MX = 36

local HOUR_Y, HOUR_H = 306, 132
local DAY_Y, DAY_H = 446, 132
local CARD_X, CARD_W = 28, 968
local HCOL_W, HCOL_X0 = 80, 32       -- 12 columns x 80 = 960
local DCOL_W, DCOL_X0 = 137, 32      -- 7 columns x 137 = 959
local ICON_PX = 38                   -- hourly / daily icon size
local HERO_ICON_PX = 150
local TEMP_PX = 116

-- Icon glyphs, injected into ui_cn.ttf as Private-Use characters.
local ICON = {
  sun     = "\u{E000}",
  partly  = "\u{E001}",
  cloud   = "\u{E002}",
  rain    = "\u{E003}",
  heavy   = "\u{E004}",
  thunder = "\u{E005}",
  snow    = "\u{E006}",
  fog     = "\u{E007}",
  drizzle = "\u{E008}",
  moon    = "\u{E009}",
}

-- Per-weather tints. Colour here carries meaning rather than decoration: gold
-- means the sun is out, blue means water is falling, white means snow. That is
-- also why the palette is small -- twenty icons on screen stay coherent only if
-- each hue maps to exactly one idea.
local IC = {
  sun     = 0xffc247,
  moon    = 0xb9d0f5,
  partly  = 0xffd06b,
  cloud   = 0xafc4dc,
  rain    = 0x5cb4ee,
  heavy   = 0x3f93d6,
  thunder = 0xffcf5b,
  snow    = 0xd2e9ff,
  fog     = 0x9fb2c8,
  drizzle = 0x74c6ee,
}

-- hexmix lives further down with the rest of the colour helpers, so the icon
-- tints get their own tiny converter up here instead of a forward reference.
local function hexc(n) return string.format("#%06x", n) end

local function lighten(h, k)
  local n = tonumber(h:sub(2), 16)
  local r, g, b = math.floor(n / 65536) % 256, math.floor(n / 256) % 256, n % 256
  return string.format("#%02x%02x%02x",
    math.floor(r + (255 - r) * k + 0.5),
    math.floor(g + (255 - g) * k + 0.5),
    math.floor(b + (255 - b) * k + 0.5))
end

-- Hero layers are large enough that full saturation reads as a sticker, so each
-- is mixed a quarter of the way to white.
local HSUN   = lighten(hexc(IC.sun), 0.16)
local HMOON  = lighten(hexc(IC.moon), 0.20)
local HCLOUD = lighten(hexc(IC.cloud), 0.22)

-- WMO code -> silhouette + tint. Night only swaps the two clear-sky cases,
-- because a sun icon at 23:00 is the kind of detail that makes a screen feel
-- dumb. Returns two values; callers must bind them before touching a label,
-- otherwise the tint would be passed to set_text as a second argument.
local function wx_icon(code, night)
  local function pick(glyph, tint) return glyph, hexc(tint) end
  -- Do not write `night and pick(...) or pick(...)`: and/or truncates the
  -- call to a single value, which silently drops the tint (nil -> crash).
  if code == 0 or code == 1 then
    if night then return pick(ICON.moon, IC.moon) end
    return pick(ICON.sun, IC.sun)
  end
  if code == 2 then
    if night then return pick(ICON.cloud, IC.cloud) end
    return pick(ICON.partly, IC.partly)
  end
  if code == 3 then return pick(ICON.cloud, IC.cloud) end
  if code == 45 or code == 48 then return pick(ICON.fog, IC.fog) end
  if code >= 51 and code <= 57 then return pick(ICON.drizzle, IC.drizzle) end
  if code == 65 or code == 82 then return pick(ICON.heavy, IC.heavy) end
  if code >= 61 and code <= 67 then return pick(ICON.rain, IC.rain) end
  if code >= 71 and code <= 77 then return pick(ICON.snow, IC.snow) end
  if code == 80 or code == 81 then return pick(ICON.rain, IC.rain) end
  if code == 85 or code == 86 then return pick(ICON.snow, IC.snow) end
  if code >= 95 then return pick(ICON.thunder, IC.thunder) end
  return pick(ICON.cloud, IC.cloud)
end

local C = {
  text   = "#f6faff",
  sub    = "#c6d4e8",
  dim    = "#93a4bd",
  faint  = "#7285a0",
  card   = "#0a1424",
  edge   = "#334d73",
  accent = "#7fd4ff",
  track  = "#14263c",
  warn   = "#ffcf6b",
  hot    = "#ffb37a",   -- daily high: warm, always
  cold   = "#7fd4ff",   -- daily low: cool, always
  htemp  = "#dde8f7",   -- hourly reading, brighter than body text
}

-- ---------------------------------------------------------------- helpers
local function I(v) return math.floor(v + 0.5) end

-- Digits are tabular at 0.58 em in the bundled subset, minus at 0.364 em, so
-- the width of a temperature string is exact rather than estimated.
local function num_w(s, px)
  local w = 0
  for i = 1, #s do
    if s:sub(i, i) == "-" then w = w + 0.364 * px else w = w + 0.58 * px end
  end
  return I(w)
end

-- ---------------------------------------------------------------- colour
local function hexmix(a, b, k)
  local ar, ag, ab = math.floor(a / 65536) % 256, math.floor(a / 256) % 256, a % 256
  local br, bg, bb = math.floor(b / 65536) % 256, math.floor(b / 256) % 256, b % 256
  return string.format("#%02x%02x%02x",
    I(ar + (br - ar) * k), I(ag + (bg - ag) * k), I(ab + (bb - ab) * k))
end

-- The hero number is the one element that must never look tinted for its own
-- sake, so its warmth is bounded: a hair cool below 0, a hair warm above 25,
-- plain white in between. At 18C it is white to the eye, at -8 it reads cold.
local function hero_tint(t)
  if t >= 8 then
    local k = (t - 8) / 16
    if k > 1 then k = 1 end
    return hexmix(0xf6faff, 0xffeedd, k)
  end
  local k = (8 - t) / 20
  if k > 1 then k = 1 end
  return hexmix(0xf6faff, 0xdcefff, k)
end

-- ---------------------------------------------------------------- text maps
local WX = {
  [0] = "晴", [1] = "少云", [2] = "多云", [3] = "阴",
  [45] = "雾", [48] = "雾凇",
  [51] = "毛毛雨", [53] = "毛毛雨", [55] = "毛毛雨",
  [56] = "冻雨", [57] = "冻雨",
  [61] = "小雨", [63] = "中雨", [65] = "大雨",
  [66] = "冻雨", [67] = "冻雨",
  [71] = "小雪", [73] = "中雪", [75] = "大雪", [77] = "米雪",
  [80] = "阵雨", [81] = "阵雨", [82] = "暴雨",
  [85] = "阵雪", [86] = "阵雪",
  [95] = "雷阵雨", [96] = "雷暴", [99] = "雷暴",
}
local function wx_text(code) return WX[code] or "未知" end

local DIRS = { "北", "东北", "东", "东南", "南", "西南", "西", "西北" }
local function dir_text(deg)
  if not deg then return "" end
  return DIRS[math.floor((deg + 22.5) / 45) % 8 + 1]
end

local WD = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" }

local function day_label(date_str, idx)
  if idx == 1 then return "今天" end
  if idx == 2 then return "明天" end
  local y, m, d = date_str:match("(%d%d%d%d)%-(%d%d)%-(%d%d)")
  if not y then return "--" end
  local ok, t = pcall(os.time, { year = tonumber(y), month = tonumber(m), day = tonumber(d) })
  if ok and type(t) == "number" and t > 0 then
    local ok2, tb = pcall(os.date, "*t", t)
    if ok2 and type(tb) == "table" and tb.wday and WD[tb.wday] then return WD[tb.wday] end
  end
  return (tonumber(d) or 0) .. "日"
end

-- ---------------------------------------------------------------- payload
-- A null column must still occupy a slot, otherwise precipitation_probability
-- would shift every later value by one.
local function num_arr(s)
  local t = {}
  for tok in s:gmatch("[^,]+") do t[#t + 1] = tonumber(tok) or 0 end
  return t
end

local function str_arr(s)
  local t = {}
  for v in s:gmatch('"([^"]*)"') do t[#t + 1] = v end
  return t
end

local function field(block, key)
  return tonumber(block:match('"' .. key .. '":(%-?%d+%.?%d*)'))
end

local function parse_weather(body)
  local cur = body:match('"current":(%b{})')
  local hb = body:match('"hourly":(%b{})')
  local db = body:match('"daily":(%b{})')
  if not cur or not hb or not db then return nil end

  local d = {}
  d.temp  = field(cur, "temperature_2m")
  d.feels = field(cur, "apparent_temperature")
  d.hum   = field(cur, "relative_humidity_2m")
  d.wind  = field(cur, "wind_speed_10m")
  d.wdir  = field(cur, "wind_direction_10m")
  d.pres  = field(cur, "pressure_msl")
  d.code  = field(cur, "weather_code")
  d.now   = cur:match('"time":"([^"]+)"')

  local ht = hb:match('"time":%[(.-)%]')
  local htemp = hb:match('"temperature_2m":%[(.-)%]')
  local hpop = hb:match('"precipitation_probability":%[(.-)%]')
  local hcode = hb:match('"weather_code":%[(.-)%]')
  if not (ht and htemp) then return nil end
  d.h_time = str_arr(ht)
  d.h_temp = num_arr(htemp)
  d.h_pop = hpop and num_arr(hpop) or {}
  d.h_code = hcode and num_arr(hcode) or {}

  local dt = db:match('"time":%[(.-)%]')
  local dmax = db:match('"temperature_2m_max":%[(.-)%]')
  local dmin = db:match('"temperature_2m_min":%[(.-)%]')
  local dcode = db:match('"weather_code":%[(.-)%]')
  local drise = db:match('"sunrise":%[(.-)%]')
  local dset = db:match('"sunset":%[(.-)%]')
  if not (dt and dmax and dmin) then return nil end
  d.d_time = str_arr(dt)
  d.d_max = num_arr(dmax)
  d.d_min = num_arr(dmin)
  d.d_code = dcode and num_arr(dcode) or {}
  d.sunrise = drise and str_arr(drise) or {}
  d.sunset = dset and str_arr(dset) or {}

  if not d.temp or #d.h_temp < 2 then return nil end
  return d
end

-- ---------------------------------------------------------------- runtime
lvgl.init({ buffer_lines = 40, tick_ms = 5, task_period_ms = 10 })

local function load_font(px, cache)
  local ok, font = pcall(lvgl.font_load, app_dir .. "/assets/ui_cn.ttf",
                         { size = px, cache_size = cache })
  if ok and font then return font end
  return nil
end

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#070d16", pad = 0 })

-- ---------------------------------------------------------------- background
-- Order matters. The backdrop is decoded and uploaded *before* any font is
-- loaded: the SD volume runs with max_files=5, and seven font loads first can
-- leave image.load_file() without a descriptor. Peak RAM is also lower here.
local diag_buf = {}
local function diag(text) diag_buf[#diag_buf + 1] = text end
local function diag_flush()
  local text = table.concat(diag_buf, "\n") .. "\n"
  for _, p in ipairs({ "/fatfs/claw/data/weather_bg.log",
                       "/sdcard/apps/weather_panel/bg.log",
                       "data/weather_bg.log" }) do
    local ok, f = pcall(io.open, p, "w")
    if ok and f then
      pcall(f.write, f, text)
      pcall(f.close, f)
      return
    end
  end
end

-- Try the package copy first, then the SD library path used by older builds.
local BG_PATHS = {
  app_dir .. "/assets/bg.jpg",
  "/sdcard/panel-assets/weather_panel/img/bg.jpg",
}

local function create_bg_object()
  local ok, obj = pcall(lvgl.image, scr, { x = 0, y = 0, w = W, h = H, pad = 0 })
  if not ok or obj == nil then return nil, tostring(obj) end
  obj:clear_flag("CLICKABLE")
  return obj
end

local function load_bg_file(path)
  local ok1, frame = pcall(image.load_file, path)
  if not ok1 then return "load err: " .. tostring(frame) end
  if frame == nil then return "load nil" end
  local obj, werr = create_bg_object()
  if not obj then
    pcall(frame.release, frame)
    return "widget err: " .. tostring(werr)
  end
  -- set_frame converts to RGB565 once and keeps the store alive, so the Lua
  -- view can be released immediately and no 1.2 MB string is created.
  local ok2 = pcall(obj.set_frame, obj, frame)
  pcall(frame.release, frame)
  if not ok2 then return "set_frame err" end
  return "ok"
end

local function build_bg()
  if not (ok_img and image) then return "no image module" end
  local why = ""
  for _, path in ipairs(BG_PATHS) do
    local st = load_bg_file(path)
    if st == "ok" then
      diag("bg=ok  src=" .. path)
      return "ok"
    end
    why = why .. (why == "" and "" or " | ") .. path:match("([^/]+/[^/]+)$") ..
          " -> " .. st
  end
  return why
end

-- Banded gradient fallback: if the JPEG cannot be read the screen still has a
-- sky, built from plain containers so nothing can fail to decode.
local function build_fallback_bg()
  local BANDS = 60                       -- 10 px slices: steps read as a gradient
  local bh = H // BANDS + 1
  for i = 1, BANDS do
    local k = (i - 1) / (BANDS - 1)
    local col
    if k < 0.5 then
      col = hexmix(0x081020, 0x14304d, k / 0.5)
    else
      col = hexmix(0x14304d, 0x080f1a, (k - 0.5) / 0.5)
    end
    lvgl.container(scr, {
      x = 0, y = (i - 1) * bh, w = W, h = bh,
      bg_color = col, radius = 0, border_width = 0, pad = 0,
    }):clear_flag("SCROLLABLE")
  end
end

local bg_status = build_bg()
if bg_status ~= "ok" then build_fallback_bg() end
diag("bg=" .. bg_status)
diag("dir=" .. app_dir .. "  image_module=" .. tostring(ok_img))

local f_temp  = load_font(TEMP_PX, 48)
local f_title = load_font(32, 128)
local f_body  = load_font(18, 160)
local f_small = load_font(15, 200)
local f_icon  = load_font(ICON_PX, 96)
local f_hero  = load_font(HERO_ICON_PX, 24)
local f_hero2 = load_font(132, 24)
if f_body then lvgl.set_default_font(f_body) end
diag("fonts=" .. table.concat({
  tostring(f_temp ~= nil), tostring(f_title ~= nil), tostring(f_body ~= nil),
  tostring(f_small ~= nil), tostring(f_icon ~= nil), tostring(f_hero ~= nil),
  tostring(f_hero2 ~= nil) }, ","))
diag_flush()

local scrim = lvgl.container(scr, {
  x = 0, y = 0, w = W, h = H,
  bg_color = "#050b16", bg_opa = 40,
  border_width = 0, pad = 0,
})
scrim:clear_flag("SCROLLABLE")
scrim:clear_flag("CLICKABLE")

-- ---------------------------------------------------------------- header
lvgl.label(scr, { text = CITY.name, x = MX, y = 22,
                  text_color = C.text, font = f_title })

local date_lbl = lvgl.label(scr, { text = "--", x = MX, y = 60,
                                   text_color = C.dim, font = f_small })
local upd_lbl = lvgl.label(scr, { text = "更新中", align = "top_right", x = -MX, y = 24,
                                  text_color = C.faint, font = f_small })
local src_lbl = lvgl.label(scr, { text = "Open-Meteo", align = "top_right", x = -MX, y = 46,
                                  text_color = C.faint, font = f_small })

-- ---------------------------------------------------------------- hero
local temp_lbl = lvgl.label(scr, { text = "--", x = MX, y = 82,
                                   text_color = C.text, font = f_temp })
local cond_lbl = lvgl.label(scr, { text = "--", x = MX, y = 204,
                                   text_color = C.sub, font = f_title })
local meta_lbl = lvgl.label(scr, { text = "--", x = MX, y = 246,
                                   text_color = C.dim, font = f_body })
local sun_lbl = lvgl.label(scr, { text = "--", x = MX, y = 274,
                                  text_color = C.faint, font = f_small })

-- Hero silhouette, optically aligned with the temperature block above it.
local hero_sun = lvgl.label(scr, { text = "", x = 276, y = 12,
                                   w = 132, h = 140,
                                   text_color = IC.sun, font = f_hero2 })

local hero_icon = lvgl.label(scr, { text = "", x = 232, y = 58,
                                    w = HERO_ICON_PX, h = 180,
                                    text_color = C.sub, font = f_hero })

-- The degree sign cannot be moved after creation, so one label is parked at
-- each possible number width and only the matching one is shown.
local degs = {}
do
  local widths = { num_w("8", TEMP_PX), num_w("18", TEMP_PX),
                   num_w("-3", TEMP_PX), num_w("-12", TEMP_PX) }
  for i = 1, 4 do
    degs[i] = lvgl.label(scr, { text = "", x = MX + widths[i] - 4, y = 98,
                                text_color = C.sub, font = f_title })
  end
end

-- ---------------------------------------------------------------- panels
local function panel(y, h)
  local p = lvgl.container(scr, {
    x = CARD_X, y = y, w = CARD_W, h = h,
    bg_color = C.card, bg_opa = 150, radius = 10,
    border_color = C.edge, border_width = 1, pad = 0,
  })
  p:clear_flag("SCROLLABLE")
  p:clear_flag("CLICKABLE")
  return p
end

panel(HOUR_Y, HOUR_H)
panel(DAY_Y, DAY_H)

-- ---------------------------------------------------------------- hourly strip
-- 12 columns, one every two hours: hour / icon / temperature / rain chance.
-- Every label spans the full column width so its content centres itself.
local hours = {}
for i = 1, 12 do
  local cx = HCOL_X0 + (i - 1) * HCOL_W
  hours[i] = {
    lbl  = lvgl.label(scr, { text = "", x = cx, y = HOUR_Y + 12,
                             w = HCOL_W, h = 18,
                             text_color = C.dim, font = f_small }),
    icon = lvgl.label(scr, { text = "", x = cx, y = HOUR_Y + 31,
                             w = HCOL_W, h = 46,
                             text_color = C.sub, font = f_icon }),
    temp = lvgl.label(scr, { text = "", x = cx, y = HOUR_Y + 78,
                             w = HCOL_W, h = 22,
                             text_color = C.sub, font = f_body }),
    pop  = lvgl.label(scr, { text = "", x = cx, y = HOUR_Y + 107,
                             w = HCOL_W, h = 18,
                             text_color = C.accent, font = f_small }),
  }
end

-- ---------------------------------------------------------------- daily strip
-- The first cell is lit so the eye has somewhere to land; bg_opa is one of the
-- few style keys that survive after creation.
local today_bg = lvgl.container(scr, {
  x = DCOL_X0 - 2, y = DAY_Y + 6, w = DCOL_W - 4, h = DAY_H - 12,
  bg_color = "#1b2c47", bg_opa = 110, radius = 8, border_width = 0, pad = 0,
})
today_bg:clear_flag("SCROLLABLE")
today_bg:clear_flag("CLICKABLE")

local days = {}
for i = 1, 7 do
  local cx = DCOL_X0 + (i - 1) * DCOL_W
  days[i] = {
    lbl  = lvgl.label(scr, { text = "", x = cx, y = DAY_Y + 16,
                             w = DCOL_W, h = 18,
                             text_color = C.dim, font = f_small }),
    icon = lvgl.label(scr, { text = "", x = cx, y = DAY_Y + 38,
                             w = DCOL_W, h = 46,
                             text_color = C.sub, font = f_icon }),
    hi   = lvgl.label(scr, { text = "", x = cx + 5, y = DAY_Y + 90,
                             w = 63, h = 22,
                             text_color = C.hot, font = f_body }),
    lo   = lvgl.label(scr, { text = "", x = cx + 69, y = DAY_Y + 90,
                             w = 63, h = 22,
                             text_color = C.cold, font = f_body }),
  }
end

-- ---------------------------------------------------------------- state
local next_at = 0
local last_ok_at = 0

-- Transparent full-screen catcher: any tap forces a refresh.
local tap = lvgl.container(scr, {
  x = 0, y = 0, w = W, h = H,
  bg_opa = 0, border_width = 0, pad = 0,
})
tap:clear_flag("SCROLLABLE")
tap:add_flag("CLICKABLE")
tap:on("clicked", function() next_at = 0 end)

-- ---------------------------------------------------------------- render
local function set_scrim(night)
  -- Just enough scrim for text contrast; the old 140/90 buried the artwork.
  if night then
    scrim:set_style({ bg_color = "#03070f", bg_opa = 105 })
  else
    scrim:set_style({ bg_color = "#06101f", bg_opa = 65 })
  end
end

local function render(d)
  local now = d.now or ""
  local hh = tonumber(now:sub(12, 13)) or 0

  local y, m, dd = now:match("(%d%d%d%d)%-(%d%d)%-(%d%d)")
  local ok_t, tb = pcall(os.date, "*t")
  local wd = ""
  if ok_t and type(tb) == "table" and tb.wday then wd = WD[tb.wday] or "" end
  if y then
    date_lbl:set_text(string.format("%d月%d日  %s",
      tonumber(m), tonumber(dd), wd))
  end

  -- hero
  local t = I(d.temp or 0)
  local ts = string.format("%d", t)
  temp_lbl:set_text(ts)
  temp_lbl:set_style({ text_color = hero_tint(t) })
  for i = 1, 4 do degs[i]:set_text("") end
  local pick = 1
  if #ts == 2 then pick = 2 end
  if ts:sub(1, 1) == "-" then pick = (#ts == 2) and 3 or 4 end
  if pick > 4 then pick = 4 end
  degs[pick]:set_text("°")

  cond_lbl:set_text(wx_text(d.code))
  meta_lbl:set_text(string.format("体感 %d°   湿度 %d%%   %s风 %d km/h   %d hPa",
    I(d.feels or 0), I(d.hum or 0), dir_text(d.wdir),
    I(d.wind or 0), I(d.pres or 0)))

  local rise = (d.sunrise[1] or ""):sub(12, 16)
  local set = (d.sunset[1] or ""):sub(12, 16)
  sun_lbl:set_text(string.format("日出 %s   日落 %s",
    (rise ~= "") and rise or "--", (set ~= "") and set or "--"))

  local rh, sh = tonumber(rise:sub(1, 2)), tonumber(set:sub(1, 2))
  local night = false
  if rh and sh then night = (hh < rh) or (hh >= sh) end
  set_scrim(night)

  -- A sun icon after sunset is the giveaway that nothing was thought through.
  local function night_at(h)
    if not (rh and sh) then return false end
    return (h < rh) or (h >= sh)
  end
  local hero_g, hero_c = wx_icon(d.code or -1, night)
  hero_c = hero_c or hexc(IC.cloud)   -- tint is a string, not the raw constant
  -- "Partly cloudy" is drawn as two stacked labels: a sun/moon behind, a cloud
  -- in front. One merged glyph would have to be a single colour and the sun
  -- would vanish into the cloud.
  if (d.code or -1) == 2 then
    if night then
      hero_sun:set_text(ICON.moon)
      hero_sun:set_style({ text_color = HMOON })
    else
      hero_sun:set_text(ICON.sun)
      hero_sun:set_style({ text_color = HSUN })
    end
    hero_icon:set_text(ICON.cloud)
    hero_icon:set_style({ text_color = HCLOUD })
  else
    hero_sun:set_text("")
    hero_icon:set_text(hero_g)
    hero_icon:set_style({ text_color = lighten(hero_c, 0.30) })
  end
  -- Condition text borrows the icon's hue, so "晴" and the sun read as one thing
  -- rather than a grey caption under a coloured picture.
  cond_lbl:set_style({ text_color = hero_c })

  -- hourly
  local ht = d.h_temp
  local htime = d.h_time
  local hpop = d.h_pop
  local hcode = d.h_code

  -- Open-Meteo returns the day from 00:00, so the strip must start at the
  -- current hour instead of at midnight, otherwise "12 hours ahead" silently
  -- shows hours that have already passed.
  local start = 1
  for k = 1, #htime do
    local hk = tonumber((htime[k] or ""):sub(12, 13))
    if hk and hk >= hh then start = k break end
  end

  for i = 1, 12 do
    local e = hours[i]
    local idx = start + (i - 1) * 2
    local v = ht[idx]
    if v then
      local h2 = tonumber((htime[idx] or ""):sub(12, 13)) or 0
      e.lbl:set_text(string.format("%d时", h2))
      local ig, ic = wx_icon(hcode[idx] or -1, night_at(h2))
      e.icon:set_text(ig)
      e.icon:set_style({ text_color = ic })
      e.temp:set_text(string.format("%d°", I(v)))
      local p = hpop[idx] or 0
      if p >= 20 then
        e.pop:set_text(string.format("%d%%", I(p)))
      else
        e.pop:set_text("")
      end
    else
      e.lbl:set_text("")
      e.icon:set_text("")
      e.temp:set_text("")
      e.pop:set_text("")
    end
  end

  -- daily
  local dmax, dmin = d.d_max, d.d_min
  for i = 1, 7 do
    local e = days[i]
    local mx, mn = dmax[i], dmin[i]
    if mx and mn then
      e.lbl:set_text(day_label(d.d_time[i] or "", i))
      local ig, ic = wx_icon(d.d_code[i] or -1, false)
      e.icon:set_text(ig)
      e.icon:set_style({ text_color = ic })
      e.hi:set_text(string.format("%d°", I(mx)))
      e.lo:set_text(string.format("%d°", I(mn)))
    else
      e.lbl:set_text("--")
      e.icon:set_text("")
      e.hi:set_text("")
      e.lo:set_text("")
    end
  end
end

-- ---------------------------------------------------------------- fetch
local URL = string.format(
  "http://api.open-meteo.com/v1/forecast?latitude=%.2f&longitude=%.2f" ..
  "&current=temperature_2m,relative_humidity_2m,apparent_temperature," ..
  "weather_code,wind_speed_10m,wind_direction_10m,pressure_msl" ..
  "&hourly=temperature_2m,weather_code,precipitation_probability" ..
  "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset" ..
  "&timezone=Asia%%2FShanghai&forecast_days=7&forecast_hours=48",
  CITY.lat, CITY.lon)

local function refresh(now)
  if not (ok_net and net) then
    upd_lbl:set_text("固件无网络模块")
    upd_lbl:set_style({ text_color = C.warn })
    return
  end

  local ok, r = pcall(net.http.request, {
    url = URL,
    method = "GET",
    headers = { ["User-Agent"] = "Mozilla/5.0 (esp32-p4 panel)" },
    timeout_ms = 8000,
    max_body = 16384,
  })

  if not ok or type(r) ~= "table" or r.status ~= 200 or not r.body then
    if last_ok_at > 0 then
      upd_lbl:set_text("离线 · 用上次数据")
    else
      upd_lbl:set_text("连接失败 · 轻触重试")
    end
    upd_lbl:set_style({ text_color = C.warn })
    next_at = now + 60000
    return
  end

  local d = parse_weather(r.body)
  if not d then
    upd_lbl:set_text("返回格式异常")
    upd_lbl:set_style({ text_color = C.warn })
    next_at = now + 120000
    return
  end

  render(d)
  last_ok_at = now
  upd_lbl:set_text(string.format("更新 %s", (d.now or ""):sub(12, 16)))
  upd_lbl:set_style({ text_color = C.faint })
  -- The label doubles as the fallback banner: if the illustration could not be
  -- decoded the reason is printed here instead of a silent downgrade.
  src_lbl:set_text(bg_status == "ok" and "Open-Meteo · 插画背景"
                                       or ("Open-Meteo · 降级 " .. bg_status):sub(1, 70))
  next_at = now + REFRESH_MS
end

-- ---------------------------------------------------------------- loop
scr:load()

while not claw.abort_requested() do
  local now = system.millis()
  if now >= next_at then
    refresh(now)
  end
  lvgl.process_events(25)
end

lvgl.deinit()
