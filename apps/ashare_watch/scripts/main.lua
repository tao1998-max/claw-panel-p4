-- A股盯盘 (ashare_watch) for the 1024x600 ESP32-P4 panel.
--
-- Two views:
--   page_main   - index cards + watch list (qt.gtimg.cn, one request, plain HTTP)
--   page_detail - intraday chart for one symbol (web.ifzq.gtimg.cn minute JSON)
--
-- Chinese names are stored locally: the quote payload is GBK and is never
-- decoded, only the ASCII numeric fields are parsed. The minute payload is
-- UTF-8 but only the leading "HHMM price" columns are read, so no decoding
-- happens there either.
--
-- The chart is drawn into a reusable RGB565 canvas with the C drawing API
-- (image.new + fill_rect) and handed to LVGL with set_frame(); the old path
-- built ~320 Lua strings per refresh and copied 588 KiB into LVGL.

local lvgl = require("lvgl")
local system = require("system")
local ok_net, net = pcall(require, "net")
local ok_image, image = pcall(require, "image")

local app_src = debug.getinfo(1, "S").source or ""
if app_src:sub(1, 1) == "@" then
  app_src = app_src:sub(2)
end
local app_dir = app_src:match("^(.*)/scripts/[^/]+$") or
                "/fatfs/claw/data/apps/ashare_watch"

-- ---------------------------------------------------------------- palette
local C = {
  bg       = "#05070d",
  card     = "#0f1521",
  rule     = "#202c3d",
  rule2    = "#1a2432",
  border   = "#2a3a52",
  text     = "#eef3ff",
  muted    = "#b3c1d6",
  dim      = "#76859e",
  up       = "#ff3b4e",
  up_soft  = "#d4677a",
  up_dark  = "#6e1922",
  dn       = "#00c98d",
  dn_soft  = "#4fb89e",
  dn_dark  = "#0d4a3a",
  flat     = "#a9b8cf",
  flat_dim = "#707f97",
  accent   = "#4de0ff",
  warn     = "#ffcf6b",
}

-- ---------------------------------------------------------------- layout
local W = 1024
local MARGIN     = 32
local CARD_W     = 306
local CARD_H     = 160
local CARD_GAP   = 21
local CARD_Y     = 76
local ROW_W      = W - MARGIN * 2
local ROW_Y0     = 274
local ROW_H      = 46
local REFRESH_OPEN_MS = 10000
local REFRESH_IDLE_MS = 120000

-- chart geometry (detail page)
local CH_W, CH_H = 920, 320
local CH_X, CH_Y = 64, 104

-- ---------------------------------------------------------------- symbols
local INDEX = {
  { key = "sh000001", name = "上证指数", code = "000001" },
  { key = "sz399001", name = "深证成指", code = "399001" },
  { key = "sz399006", name = "创业板指", code = "399006" },
}

local STOCKS = {
  { key = "sh600519", name = "贵州茅台", code = "600519" },
  { key = "sz300750", name = "宁德时代", code = "300750" },
  { key = "sh688981", name = "中芯国际", code = "688981" },
  { key = "sz002594", name = "比亚迪",   code = "002594" },
  { key = "sh600036", name = "招商银行", code = "600036" },
  { key = "sh601318", name = "中国平安", code = "601318" },
  { key = "sh588080", name = "科创50ETF易方达", code = "588080" },
}

local function build_url()
  local parts = {}
  for _, it in ipairs(INDEX) do parts[#parts + 1] = it.key end
  for _, it in ipairs(STOCKS) do parts[#parts + 1] = it.key end
  return "http://qt.gtimg.cn/q=" .. table.concat(parts, ",")
end
local QUOTE_URL = build_url()

-- ---------------------------------------------------------------- helpers
local function split(s, sep)
  local out = {}
  local from = 1
  while true do
    local i = string.find(s, sep, from, true)
    if not i then
      out[#out + 1] = string.sub(s, from)
      break
    end
    out[#out + 1] = string.sub(s, from, i - 1)
    from = i + #sep
  end
  return out
end

local function fmt_price(v)
  if not v or v <= 0 then return "--" end
  -- ETFs and low-priced stocks quote in thousandths; two decimals would
  -- silently round 1.261 to 1.26.
  if v < 5 then return string.format("%.3f", v) end
  return string.format("%.2f", v)
end

local function fmt_pct(v)
  if not v then return "--" end
  if v > 0.0001 then return string.format("+%.2f%%", v) end
  if v < -0.0001 then return string.format("%.2f%%", v) end
  return "0.00%"
end

local function fmt_chg(v)
  if not v then return "--" end
  if v > 0.0001 then return string.format("+%.2f", v) end
  if v < -0.0001 then return string.format("%.2f", v) end
  return "0.00"
end

local function fmt_vol(v)
  if not v or v <= 0 then return "--" end
  if v >= 100000000 then return string.format("%.2f亿手", v / 100000000) end
  if v >= 10000 then return string.format("%.2f万手", v / 10000) end
  return string.format("%.0f手", v)
end

local function fmt_amt(v)
  if not v or v <= 0 then return "--" end
  if v >= 100000000 then return string.format("%.2f亿", v / 100000000) end
  if v >= 10000 then return string.format("%.2f万", v / 10000) end
  return string.format("%.0f", v)
end

local function tone(v)
  if not v then return C.flat, C.flat_dim end
  if v > 0.0001 then return C.up, C.up_soft end
  if v < -0.0001 then return C.dn, C.dn_soft end
  return C.flat, C.flat_dim
end

-- Returns "open" / "closed" / "unknown". Without a synced RTC we treat the
-- clock as unknown and keep polling, rather than freezing on a false close.
local function market_state()
  local ok, t = pcall(os.date, "*t")
  if not ok or type(t) ~= "table" or not t.year or t.year < 2020 then
    return "unknown"
  end
  if t.wday == 1 or t.wday == 7 then return "closed" end
  local mins = t.hour * 60 + t.min
  if (mins >= 570 and mins < 690) or (mins >= 780 and mins < 900) then
    return "open"
  end
  return "closed"
end

-- ---------------------------------------------------------------- chart
-- Drawn with the C canvas API. 920x320 RGB565 = 588 KiB, allocated once and
-- reused for every tap; no Lua pixel strings, no per-refresh copy.
local CK = {
  bg      = 0x0a0f17,
  bg_next = 0x06090e,
  vgrid   = 0x131b26,
  grid    = 0x18222f,
  prev    = 0x33415a,
  up_fill = 0x251014,
  dn_fill = 0x08201a,
  up_line = 0xff3b4e,
  dn_line = 0x00c98d,
  flat    = 0x8fa2bb,
  head    = 0xdfe9fa,
}

local VGRID_SLOTS = { 60, 120, 180 }   -- 10:30 / 11:30 / 14:00 on a 241 slot day

-- Draw the intraday curve into a reusable canvas. Returns ymin, ymax, or nil
-- when there is nothing to draw.
local function render_chart(pts, prev, canvas)
  local n = #pts
  if n < 2 then return nil end

  local mn, mx = pts[1], pts[1]
  for i = 2, n do
    local p = pts[i]
    if p < mn then mn = p end
    if p > mx then mx = p end
  end

  local base = prev
  if not base or base <= 0 then base = pts[1] end

  -- Symmetric range around the previous close: the reference line stays at
  -- mid height, which is what an intraday chart is supposed to look like.
  local r = mx - base
  if base - mn > r then r = base - mn end
  local floor_r = base * 0.004
  if r < floor_r then r = floor_r end

  local ymin, ymax = base - r, base + r
  local span = ymax - ymin

  local function yof(p)
    local y = math.floor((ymax - p) / span * (CH_H - 1) + 0.5)
    if y < 0 then y = 0 elseif y > CH_H - 1 then y = CH_H - 1 end
    return y
  end

  local ys = {}
  for i = 1, n do ys[i] = yof(pts[i]) end
  local y0 = yof(base)

  -- Map 241 trading-minute slots onto the fixed full-day axis.
  local ycol = {}
  local x_end = 0
  for i = 1, n do
    local x = math.floor((i - 1) * (CH_W - 1) / 240 + 0.5)
    if x > CH_W - 1 then x = CH_W - 1 end
    ycol[x] = ys[i]
    if x > x_end then x_end = x end
  end

  local pk, pv = nil, nil
  for x = 0, x_end do
    local y = ycol[x]
    if y then
      if pk and x - pk > 1 then
        local d = x - pk
        for c = pk + 1, x - 1 do
          ycol[c] = math.floor(pv + (y - pv) * (c - pk) / d + 0.5)
        end
      end
      pk, pv = x, y
    end
  end
  for x = 0, x_end do
    if not ycol[x] then ycol[x] = y0 end
  end

  -- Traded part is slightly lighter than the not-yet-reached part, so the
  -- empty right half reads as "session still running".
  local split = x_end + 1
  if split > CH_W then split = CH_W end
  canvas:fill_rect(0, 0, split, CH_H, CK.bg)
  if split < CH_W then
    canvas:fill_rect(split, 0, CH_W - split, CH_H, CK.bg_next)
  end

  for i = 1, #VGRID_SLOTS do
    local bx = math.floor(VGRID_SLOTS[i] * (CH_W - 1) / 240 + 0.5)
    canvas:fill_rect(bx, 0, 1, CH_H, CK.vgrid)
  end

  -- Area fill between the curve and the previous-close line.
  for x = 0, x_end do
    local cy = ycol[x]
    if cy < y0 then
      canvas:fill_rect(x, cy, 1, y0 - cy, CK.up_fill)
    elseif cy > y0 then
      canvas:fill_rect(x, y0 + 1, 1, cy - y0, CK.dn_fill)
    end
  end

  -- Horizontal guides, dashed previous close, then the curve.
  local q1, q3 = math.floor(CH_H * 0.25), math.floor(CH_H * 0.75)
  if q1 ~= y0 then canvas:fill_rect(0, q1, CH_W, 1, CK.grid) end
  if q3 ~= y0 then canvas:fill_rect(0, q3, CH_W, 1, CK.grid) end
  for x = 0, CH_W - 1, 2 do
    canvas:fill_rect(x, y0, 1, 1, CK.prev)
    if x + 1 < CH_W then
      canvas:fill_rect(x + 1, y0, 1, 1, CK.bg)
    end
  end

  local stroke = (pts[n] >= base) and CK.up_line or CK.dn_line
  if math.abs(pts[n] - base) < base * 0.00005 then stroke = CK.flat end
  for x = 0, x_end do
    local y1 = ycol[x]
    local y2 = (x > 0) and ycol[x - 1] or y1
    local ya, yb = y1, y2
    if ya > yb then ya, yb = yb, ya end
    canvas:fill_rect(x, ya, 2, yb - ya + 1, stroke)
  end

  -- Trailing marker so the latest minute is obvious at a glance.
  local ly = ycol[x_end]
  if ly then
    local hx = x_end - 3
    if hx < 0 then hx = 0 end
    if hx > CH_W - 7 then hx = CH_W - 7 end
    canvas:fill_rect(hx, ly - 3, 7, 7, CK.head)
  end

  return ymin, ymax
end

local chart_canvas
local function ensure_chart_canvas()
  if chart_canvas then return chart_canvas end
  if not (ok_image and image and image.new) then return nil end
  local ok, cv = pcall(image.new, CH_W, CH_H, image.RGB565)
  if not ok or not cv then return nil end
  chart_canvas = cv
  return cv
end

-- ---------------------------------------------------------------- runtime
lvgl.init({ buffer_lines = 40, tick_ms = 5, task_period_ms = 10 })

local function load_font(px, cache)
  local ok, font = pcall(lvgl.font_load, app_dir .. "/assets/ui_cn.ttf",
                         { size = px, cache_size = cache })
  if ok and font then return font end
  return nil
end

local f_huge  = load_font(40, 96)
local f_mid   = load_font(22, 160)
local f_body  = load_font(18, 160)
local f_small = load_font(15, 200)
if f_body then lvgl.set_default_font(f_body) end

local scr = lvgl.create_screen()
scr:set_style({ bg_color = C.bg, pad = 0 })

local function full_page()
  local p = lvgl.container(scr, {
    x = 0, y = 0, w = W, h = 600,
    bg_opa = 0, border_width = 0, pad = 0,
  })
  p:clear_flag("SCROLLABLE")
  return p
end

local page_main = full_page()
local page_detail = full_page()
page_detail:add_flag("HIDDEN")

local view = "main"

-- Forward declarations: the widget loops below bind click handlers that call
-- these, and the handlers only resolve them when a tap actually happens.
local open_detail, close_detail
local next_at = 0
local detail_next_at = 0

-- ---------------------------------------------------------------- header
lvgl.container(page_main, {
  x = MARGIN, y = 17, w = 4, h = 22,
  bg_color = C.accent, radius = 2, border_width = 0, pad = 0,
}):clear_flag("SCROLLABLE")

lvgl.label(page_main, { text = "A股盯盘", x = MARGIN + 13, y = 12,
                        text_color = C.text, font = f_mid })

local dot = lvgl.container(page_main, {
  x = 152, y = 25, w = 8, h = 8,
  bg_color = C.flat_dim, radius = 4, border_width = 0, pad = 0,
})
dot:clear_flag("SCROLLABLE")

local live_lbl = lvgl.label(page_main, { text = "连接中", x = 168, y = 15,
                                         text_color = C.muted, font = f_small })

local clock_lbl = lvgl.label(page_main, { text = "--:--:--", align = "top_right",
                                          x = -MARGIN, y = 8,
                                          text_color = C.muted, font = f_small })
local ago_lbl = lvgl.label(page_main, { text = "等待首次刷新", align = "top_right",
                                        x = -MARGIN, y = 29,
                                        text_color = C.dim, font = f_small })

lvgl.line(page_main, {
  points = { { x = MARGIN, y = 64 }, { x = W - MARGIN, y = 64 } },
  line_color = C.rule, line_width = 2,
})

-- ---------------------------------------------------------------- detail page
local back_btn = lvgl.container(page_detail, {
  x = 24, y = 18, w = 96, h = 40,
  bg_color = C.card, radius = 8,
  border_color = C.border, border_width = 1, pad = 0,
})
back_btn:clear_flag("SCROLLABLE")
back_btn:add_flag("CLICKABLE")
lvgl.label(back_btn, { text = "返回", align = "center", x = 0, y = 0,
                       text_color = C.muted, font = f_body })

local d_name = lvgl.label(page_detail, { text = "--", x = 136, y = 12,
                                         text_color = C.text, font = f_mid })
local d_code = lvgl.label(page_detail, { text = "--", x = 136, y = 44,
                                         text_color = C.dim, font = f_small })
local d_price = lvgl.label(page_detail, { text = "--", align = "top_right",
                                          x = -330, y = 6,
                                          text_color = C.flat, font = f_huge })
local d_chg = lvgl.label(page_detail, { text = "--", align = "top_right",
                                        x = -190, y = 20,
                                        text_color = C.flat_dim, font = f_mid })
local d_pct = lvgl.label(page_detail, { text = "--", align = "top_right",
                                        x = -32, y = 20,
                                        text_color = C.flat, font = f_mid })

lvgl.line(page_detail, {
  points = { { x = MARGIN, y = 88 }, { x = W - MARGIN, y = 88 } },
  line_color = C.rule, line_width = 2,
})

local chart_box = lvgl.container(page_detail, {
  x = CH_X - 1, y = CH_Y - 1, w = CH_W + 2, h = CH_H + 2,
  bg_color = "#0a0f17", radius = 2,
  border_color = C.rule2, border_width = 1, pad = 0,
})
chart_box:clear_flag("SCROLLABLE")
chart_box:add_flag("CLICKABLE")

local chart = lvgl.image(page_detail, { x = CH_X, y = CH_Y, w = CH_W, h = CH_H, pad = 0 })
chart:clear_flag("CLICKABLE")

-- A transparent holder keeps the status text centred whatever its length;
-- labels have no text_align, so centring has to come from the parent.
local msg_holder = lvgl.container(page_detail, {
  x = CH_X, y = CH_Y, w = CH_W, h = CH_H,
  bg_opa = 0, border_width = 0, pad = 0,
})
msg_holder:clear_flag("SCROLLABLE")

local chart_msg = lvgl.label(msg_holder, {
  text = "加载中", align = "center", x = 0, y = 0,
  text_color = C.dim, font = f_body,
})

local y_top = lvgl.label(page_detail, { text = "--", x = 6, y = CH_Y - 4,
                                        text_color = C.dim, font = f_small })
-- Every coordinate handed to LVGL must be an integer: `a / 2` is a float in
-- Lua 5.4 even when the value is whole, and the binding rejects it with
-- "lvgl option 'y' must be an integer". Use floor division / math.floor.
local y_mid = lvgl.label(page_detail, { text = "--", x = 6, y = CH_Y + CH_H // 2 - 10,
                                        text_color = C.muted, font = f_small })
local y_bot = lvgl.label(page_detail, { text = "--", x = 6, y = CH_Y + CH_H - 16,
                                        text_color = C.dim, font = f_small })

local axis_lbls = {}
do
  local items = {
    { t = "09:30", x = CH_X - 10 },
    { t = "10:30", x = CH_X + 230 - 16 },
    { t = "11:30", x = CH_X + 460 - 30 },
    { t = "14:00", x = CH_X + 690 - 16 },
    { t = "15:00", x = CH_X + CH_W - 26 },
  }
  for i, it in ipairs(items) do
    axis_lbls[i] = lvgl.label(page_detail, {
      text = it.t, x = it.x, y = CH_Y + CH_H + 8,
      text_color = C.dim, font = f_small,
    })
  end
end

lvgl.line(page_detail, {
  points = { { x = MARGIN, y = 452 }, { x = W - MARGIN, y = 452 } },
  line_color = C.rule, line_width = 2,
})

local d_stats = {}
do
  local names = { "今开", "最高", "最低", "昨收", "成交量", "成交额" }
  for i, nm in ipairs(names) do
    local bx = MARGIN + (i - 1) * 160
    lvgl.label(page_detail, { text = nm, x = bx, y = 468,
                              text_color = C.dim, font = f_small })
    d_stats[i] = lvgl.label(page_detail, { text = "--", x = bx, y = 490,
                                           text_color = C.muted, font = f_body })
  end
end

local d_hint = lvgl.label(page_detail, { text = "轻触图表区域刷新", x = MARGIN, y = 566,
                                         text_color = C.dim, font = f_small })
local d_src = lvgl.label(page_detail, { text = "腾讯行情 分时", align = "top_right",
                                        x = -MARGIN, y = 566,
                                        text_color = C.dim, font = f_small })

-- ---------------------------------------------------------------- cards
local cards = {}

for i, item in ipairs(INDEX) do
  local cx = MARGIN + (i - 1) * (CARD_W + CARD_GAP)
  local card = lvgl.container(page_main, {
    x = cx, y = CARD_Y, w = CARD_W, h = CARD_H,
    bg_color = C.card, radius = 8,
    border_color = C.border, border_width = 1, pad = 0,
  })
  card:clear_flag("SCROLLABLE")
  card:add_flag("CLICKABLE")
  card:on("clicked", function() open_detail(item) end)

  lvgl.label(card, { text = item.name, x = 16, y = 8,
                     text_color = C.muted, font = f_body })
  lvgl.label(card, { text = item.code, align = "top_right", x = -16, y = 10,
                     text_color = C.dim, font = f_small })

  local price = lvgl.label(card, { text = "--", x = 16, y = 36,
                                   text_color = C.flat, font = f_huge })
  local pct = lvgl.label(card, { text = "--", align = "top_right",
                                 x = -16, y = 62,
                                 text_color = C.flat, font = f_mid })
  local chg = lvgl.label(card, { text = "--", x = 16, y = 96,
                                 text_color = C.flat_dim, font = f_body })
  local bar = lvgl.container(card, {
    x = 16, y = CARD_H - 22, w = CARD_W - 32, h = 3,
    bg_color = C.flat_dim, radius = 1, border_width = 0, pad = 0,
  })
  bar:clear_flag("SCROLLABLE")

  cards[i] = {
    item = item, price = price, pct = pct, chg = chg, bar = bar,
    last = nil, flash_until = 0,
  }
end

-- ---------------------------------------------------------------- table head
lvgl.label(page_main, { text = "自选", x = MARGIN + 20, y = 246,
                        text_color = C.dim, font = f_small })
lvgl.label(page_main, { text = "最新", x = 618, y = 246,
                        text_color = C.dim, font = f_small })
lvgl.label(page_main, { text = "涨跌额", x = 775, y = 246,
                        text_color = C.dim, font = f_small })
lvgl.label(page_main, { text = "涨跌幅", x = 895, y = 246,
                        text_color = C.dim, font = f_small })

lvgl.line(page_main, {
  points = { { x = MARGIN, y = 272 }, { x = W - MARGIN, y = 272 } },
  line_color = C.rule, line_width = 2,
})

-- ---------------------------------------------------------------- rows
local rows = {}

for i, item in ipairs(STOCKS) do
  local ry = ROW_Y0 + (i - 1) * ROW_H
  local row = lvgl.container(page_main, {
    x = MARGIN, y = ry, w = ROW_W, h = ROW_H,
    bg_color = C.bg, radius = 0, border_width = 0, pad = 0,
  })
  row:clear_flag("SCROLLABLE")
  row:add_flag("CLICKABLE")
  row:on("clicked", function() open_detail(item) end)

  local bar = lvgl.container(row, {
    x = 0, y = 14, w = 3, h = 20,
    bg_color = C.flat_dim, radius = 1, border_width = 0, pad = 0,
  })
  bar:clear_flag("SCROLLABLE")

  lvgl.label(row, { text = item.name, align = "left_mid", x = 20, y = 0,
                    text_color = C.text, font = f_body })
  lvgl.label(row, { text = item.code, align = "left_mid", x = 210, y = 1,
                    text_color = C.dim, font = f_small })

  local price = lvgl.label(row, { text = "--", align = "right_mid", x = -300, y = 0,
                                  text_color = C.flat, font = f_mid })
  local chg = lvgl.label(row, { text = "--", align = "right_mid", x = -170, y = 0,
                                text_color = C.flat_dim, font = f_body })
  local pct = lvgl.label(row, { text = "--", align = "right_mid", x = -24, y = 0,
                                text_color = C.flat, font = f_mid })

  if i < #STOCKS then
    lvgl.line(page_main, {
      points = { { x = MARGIN + 20, y = ry + ROW_H }, { x = W - MARGIN, y = ry + ROW_H } },
      line_color = C.rule2, line_width = 1,
    })
  end

  rows[i] = { item = item, price = price, chg = chg, pct = pct, bar = bar,
              last = nil, flash_until = 0 }
end

-- ---------------------------------------------------------------- state
local last_ok_at = 0
local last_clock = ""
local dot_phase = -1
local mode = "boot"

local detail_key = nil
local detail_loaded_at = 0
local detail_need_paint = false

local function set_mode(next_mode, hint)
  mode = next_mode
  if next_mode == "live" then
    live_lbl:set_text("实时")
    live_lbl:set_style({ text_color = C.up })
  elseif next_mode == "idle" then
    live_lbl:set_text("已收盘")
    live_lbl:set_style({ text_color = C.warn })
    dot:set_style({ bg_color = C.warn })
  elseif next_mode == "offline" then
    live_lbl:set_text("离线")
    live_lbl:set_style({ text_color = C.dim })
    dot:set_style({ bg_color = C.flat_dim })
    ago_lbl:set_text(hint or "连接失败")
  end
end

-- ---------------------------------------------------------------- quote parser
local function apply_quotes(body, now)
  local hits = 0
  for key, payload in string.gmatch(body, "v_([%w_]+)=\"([^\"]*)\"") do
    local f = split(payload, "~")
    local price = tonumber(f[4])
    local prev  = tonumber(f[5])
    if price and price > 0 then
      -- Derive change from price/prev first: those two field positions are far
      -- more stable than the trailing ones. Fall back to the payload columns.
      local chg, pct
      if prev and prev > 0 then
        chg = price - prev
        pct = chg / prev * 100
      else
        chg = tonumber(f[32])
        pct = tonumber(f[33])
      end

      hits = hits + 1

      for _, c in ipairs(cards) do
        if c.item.key == key then
          if c.last and c.last ~= price then c.flash_until = now + 260 end
          c.last = price
          local up, soft = tone(pct)
          c.price:set_text(fmt_price(price))
          c.pct:set_text(fmt_pct(pct))
          c.chg:set_text(fmt_chg(chg))
          c.pct:set_style({ text_color = up })
          c.chg:set_style({ text_color = soft })
          c.bar:set_style({ bg_color = up })
          c.base = up
        end
      end

      for _, r in ipairs(rows) do
        if r.item.key == key then
          if r.last and r.last ~= price then r.flash_until = now + 260 end
          r.last = price
          local up, soft = tone(pct)
          r.price:set_text(fmt_price(price))
          r.chg:set_text(fmt_chg(chg))
          r.pct:set_text(fmt_pct(pct))
          r.price:set_style({ text_color = up })
          r.chg:set_style({ text_color = soft })
          r.pct:set_style({ text_color = up })
          r.bar:set_style({ bg_color = up })
          r.base = up
        end
      end
    end
  end
  return hits
end

-- ---------------------------------------------------------------- quote refresh
local function refresh_all(now)
  if not (ok_net and net) then
    set_mode("offline", "固件无网络模块")
    return
  end

  local ok, r = pcall(net.http.request, {
    url = QUOTE_URL,
    method = "GET",
    headers = {
      ["User-Agent"] = "Mozilla/5.0 (esp32-p4 panel)",
      ["Referer"] = "http://finance.qq.com/",
    },
    timeout_ms = 6000,
    max_body = 32768,
  })

  if not ok or type(r) ~= "table" or r.status ~= 200 or not r.body then
    set_mode("offline", "请求失败，下轮重试")
    return
  end

  local hits = apply_quotes(r.body, now)
  if hits == 0 then
    set_mode("offline", "返回格式异常")
    return
  end

  last_ok_at = now
  if market_state() == "open" then
    set_mode("live", nil)
  else
    set_mode("idle", nil)
  end
end

-- ---------------------------------------------------------------- minute data
local function parse_minute(body, key)
  local core = body:match('"data":%[(.-)%]')
  if not core then return nil end

  local pts, vols, amts = {}, {}, {}
  for t, p, v, a in core:gmatch('"(%d%d%d%d) ([%d%.]+) (%d+) ([%d%.]+)') do
    local pv = tonumber(p)
    if pv and pv > 0 then
      pts[#pts + 1] = pv
      vols[#vols + 1] = tonumber(v) or 0
      amts[#amts + 1] = tonumber(a) or 0
    end
  end
  if #pts < 2 then return nil end

  local prev, open
  local qt = body:match('"qt":(%b{})')
  if qt then
    local arr = qt:match('"' .. key .. '":(%b[])')
    if arr then
      local f = {}
      for s in arr:gmatch('"([^"]*)"') do f[#f + 1] = s end
      prev = tonumber(f[5])
      open = tonumber(f[6])
    end
  end

  return {
    pts = pts, vols = vols, amts = amts,
    prev = prev, open = open,
    vol = vols[#vols], amt = amts[#amts],
  }
end

-- ---------------------------------------------------------------- detail page
local function detail_reset(item)
  d_name:set_text(item.name)
  d_code:set_text(item.code)
  d_price:set_text("--")
  d_price:set_style({ text_color = C.flat })
  d_chg:set_text("--")
  d_chg:set_style({ text_color = C.flat_dim })
  d_pct:set_text("--")
  d_pct:set_style({ text_color = C.flat })
  y_top:set_text("--")
  y_mid:set_text("--")
  y_bot:set_text("--")
  for i = 1, 6 do d_stats[i]:set_text("--") end
  chart_msg:set_text("加载中")
  chart_msg:set_style({ text_color = C.dim })
end

function open_detail(item)
  view = "detail"
  detail_key = item.key
  detail_next_at = 0
  detail_loaded_at = 0
  detail_reset(item)
  page_main:add_flag("HIDDEN")
  page_detail:clear_flag("HIDDEN")
  -- Ask the main loop to paint once before the blocking fetch. Calling
  -- process_events() from inside a click callback would re-enter the handler.
  detail_need_paint = true
end

function close_detail()
  view = "main"
  detail_key = nil
  page_detail:add_flag("HIDDEN")
  page_main:clear_flag("HIDDEN")
  next_at = 0
end

back_btn:on("clicked", function() close_detail() end)

-- Tapping the chart pulls a fresh intraday snapshot on the spot.
chart_box:on("clicked", function()
  if view == "detail" then
    chart_msg:set_text("刷新中")
    chart_msg:set_style({ text_color = C.dim })
    detail_next_at = 0
    detail_need_paint = true
  end
end)

local function refresh_detail(now)
  if not detail_key then return end
  if not (ok_net and net) then
    chart_msg:set_text("固件无网络模块")
    return
  end

  local rnd = tostring(math.floor(now / 997) % 100000)
  local url = "http://web.ifzq.gtimg.cn/appstock/app/minute/query?code=" ..
              detail_key .. "&_var=m&r=" .. rnd

  local ok, r = pcall(net.http.request, {
    url = url,
    method = "GET",
    headers = { ["User-Agent"] = "Mozilla/5.0 (esp32-p4 panel)" },
    timeout_ms = 8000,
    max_body = 24576,
    follow_redirects = 3,
  })

  if not ok or type(r) ~= "table" or r.status ~= 200 or not r.body then
    if not detail_loaded_at or detail_loaded_at == 0 then
      chart_msg:set_text("请求失败，轻触重试")
      chart_msg:set_style({ text_color = C.warn })
    end
    detail_next_at = now + 20000
    return
  end

  local d = parse_minute(r.body, detail_key)
  if not d then
    chart_msg:set_text("返回格式异常")
    chart_msg:set_style({ text_color = C.warn })
    detail_next_at = now + 30000
    return
  end

  local cv = ensure_chart_canvas()
  if not cv then
    chart_msg:set_text("固件无画布模块")
    chart_msg:set_style({ text_color = C.warn })
    detail_next_at = now + 60000
    return
  end

  local ymin, ymax = render_chart(d.pts, d.prev, cv)
  if not ymin then
    chart_msg:set_text("暂无分时数据")
    detail_next_at = now + 60000
    return
  end

  local ok_set = pcall(chart.set_frame, chart, cv)
  if not ok_set then
    chart_msg:set_text("绘图失败")
    chart_msg:set_style({ text_color = C.warn })
    detail_next_at = now + 60000
    return
  end

  chart_msg:set_text("")

  local last = d.pts[#d.pts]
  local base = d.prev or d.open or d.pts[1]
  local chg = last - base
  local pct = (base > 0) and (chg / base * 100) or 0
  local up, soft = tone(pct)

  d_price:set_text(fmt_price(last))
  d_price:set_style({ text_color = up })
  d_chg:set_text(fmt_chg(chg))
  d_chg:set_style({ text_color = soft })
  d_pct:set_text(fmt_pct(pct))
  d_pct:set_style({ text_color = up })

  y_top:set_text(fmt_price(ymax))
  y_mid:set_text(fmt_price(base))
  y_bot:set_text(fmt_price(ymin))

  local mn, mx = d.pts[1], d.pts[1]
  for i = 2, #d.pts do
    if d.pts[i] < mn then mn = d.pts[i] end
    if d.pts[i] > mx then mx = d.pts[i] end
  end

  d_stats[1]:set_text(fmt_price(d.open or d.pts[1]))
  d_stats[2]:set_text(fmt_price(mx))
  d_stats[3]:set_text(fmt_price(mn))
  d_stats[4]:set_text(fmt_price(base))
  d_stats[5]:set_text(fmt_vol(d.vol))
  d_stats[6]:set_text(fmt_amt(d.amt))
  -- High/low keep the up/down tone of their price relative to yesterday's
  -- close, not a fixed red/green role: an all-day decline has a "high" that is
  -- still below the previous close.
  d_stats[2]:set_style({ text_color = (mx >= base) and C.up or C.dn })
  d_stats[3]:set_style({ text_color = (mn >= base) and C.up or C.dn })

  detail_loaded_at = now
  -- Deliberately a one-shot fetch; the canvas is reused but this page
  -- refreshes on tap, so no detail timer is needed.
  detail_next_at = math.huge
end

-- ---------------------------------------------------------------- loop
scr:load()

while not claw.abort_requested() do
  local now = system.millis()

  -- Paint the freshly opened detail page once, before the fetch that blocks.
  if detail_need_paint then
    detail_need_paint = false
    lvgl.process_events(30)
  end

  if view == "main" then
    if now >= next_at then
      refresh_all(now)
      local interval = REFRESH_OPEN_MS
      if mode == "offline" then
        interval = 30000
      elseif market_state() == "closed" then
        interval = REFRESH_IDLE_MS
      end
      next_at = now + interval
    end
  else
    if now >= detail_next_at then
      refresh_detail(now)
    end
  end

  if mode == "live" and view == "main" then
    local phase = math.floor(now / 700) % 2
    if phase ~= dot_phase then
      dot_phase = phase
      dot:set_style({ bg_color = (phase == 0) and C.up or C.up_dark })
    end
  end

  if view == "main" then
    for _, c in ipairs(cards) do
      if c.flash_until > 0 and now >= c.flash_until then
        c.flash_until = 0
        c.price:set_style({ text_color = c.base or C.flat })
      elseif c.flash_until > now then
        c.price:set_style({ text_color = C.text })
      end
    end
    for _, r in ipairs(rows) do
      if r.flash_until > 0 and now >= r.flash_until then
        r.flash_until = 0
        r.price:set_style({ text_color = r.base or C.flat })
      elseif r.flash_until > now then
        r.price:set_style({ text_color = C.text })
      end
    end

    if last_ok_at > 0 then
      local sec = math.floor((now - last_ok_at) / 1000)
      if sec < 1 then
        ago_lbl:set_text("刚刚刷新")
      else
        ago_lbl:set_text(string.format("%d 秒前刷新", sec))
      end
    end

    local ok_t, t = pcall(os.date, "%H:%M:%S")
    if ok_t and type(t) == "string" and t ~= last_clock then
      last_clock = t
      clock_lbl:set_text(t)
    end
  end

  lvgl.process_events(20)
end

lvgl.deinit()
