-- Countdown timer for the 1024x600 ESP32-P4 panel.
-- Chinese UI, using a small bundled TTF subset like the Blackjack app.

local lvgl = require("lvgl")
local system = require("system")

local app_src = debug.getinfo(1, "S").source or ""
if app_src:sub(1, 1) == "@" then
  app_src = app_src:sub(2)
end
local app_dir = app_src:match("^(.*)/scripts/[^/]+$") or
                "/fatfs/claw/data/apps/countdown"

local W = 1024
local PANEL_W, PANEL_H = 680, 472
local RING_SZ = 290

local C = {
  bg        = "#070b14",
  panel     = "#111a2b",
  panel_2   = "#0c1322",
  ring_bg   = "#1c2740",
  ring_fg   = "#36d1c4",
  ring_warn = "#ffb347",
  ring_done = "#ff5f6d",
  text      = "#f2f6ff",
  muted     = "#7f8ca6",
  dim       = "#55617a",
  accent    = "#36d1c4",
  btn       = "#1b2740",
  btn_edge  = "#2c3d5e",
  gold      = "#ffcf6b",
}

local PRESETS = { 60, 300, 600, 1800 }
local PICK = 2

-- ------------------------------------------------------------- helpers

local function fmt(sec)
  sec = math.max(0, math.floor(sec))
  if sec >= 3600 then
    return string.format("%02d:%02d:%02d",
                         math.floor(sec / 3600), math.floor((sec % 3600) / 60), sec % 60)
  end
  return string.format("%02d:%02d", math.floor(sec / 60), sec % 60)
end

local function preset_name(sec)
  if sec >= 3600 then
    return string.format("%d 小时", math.floor(sec / 3600))
  end
  return string.format("%d 分钟", math.floor(sec / 60))
end

-- ------------------------------------------------------------- runtime

lvgl.init({ buffer_lines = 40, tick_ms = 5, task_period_ms = 10 })

local function load_font(px, cache)
  local ok, font = pcall(lvgl.font_load,
                         app_dir .. "/assets/countdown_cn.ttf",
                         { size = px, cache_size = cache })
  if ok and font then
    return font
  end
  return nil
end

local f_ui = load_font(20, 128)
local f_digits = load_font(76, 64)
local f_title = load_font(40, 64)
if f_ui then
  lvgl.set_default_font(f_ui)
end

local state = "idle"
local total = PRESETS[PICK]
local remaining = PRESETS[PICK]
local deadline = 0
local finished_at = 0
local blink_on = false

local scr = lvgl.create_screen()
scr:set_style({ bg_color = C.bg, pad = 0 })

-- Header ---------------------------------------------------------

local title = lvgl.label(scr, {
  text = "倒计时", x = 40, y = 4, text_color = C.accent, font = f_title,
})
title:set_style({ text_color = C.accent })
lvgl.label(scr, {
  text = "选择预设，然后开始；随时暂停或继续。",
  x = 40, y = 68, w = 760, text_color = C.muted, font = f_ui,
})
local state_lbl = lvgl.label(scr, {
  text = "就绪", align = "top_right", x = -40, y = 22,
  text_color = C.muted, font = f_ui,
})
lvgl.line(scr, {
  points = { { x = 40, y = 100 }, { x = W - 40, y = 100 } },
  line_color = C.panel_2, line_width = 2,
})

-- Center panel ---------------------------------------------------

local PX, PY = (W - PANEL_W) // 2, 104
lvgl.container(scr, {
  x = PX, y = PY, w = PANEL_W, h = PANEL_H,
  bg_color = C.panel, radius = 26, border_color = C.panel_2, border_width = 2, pad = 0,
}):clear_flag("SCROLLABLE")

local RCX = PX + PANEL_W // 2
local RCY = PY + 196

local ring = lvgl.arc(scr, {
  x = RCX - RING_SZ // 2, y = RCY - RING_SZ // 2, w = RING_SZ, h = RING_SZ,
  min = 0, max = 1000, value = 1000,
  start_angle = 135, end_angle = 405,
  bg_start_angle = 135, bg_end_angle = 405,
  arc_width = 14, radius = RING_SZ,
  bg_color = C.ring_bg, line_color = C.ring_fg, border_width = 0, pad = 0,
})
ring:clear_flag("CLICKABLE")

local digits = lvgl.label(scr, {
  text = "05:00", align = "center", y = 0,
  text_color = C.text, font = f_digits,
})
digits:set_style({ text_color = C.text })

local hint = lvgl.label(scr, {
  text = "准备开始", align = "center", y = 76,
  text_color = C.muted, font = f_ui,
})

local total_lbl = lvgl.label(scr, {
  text = "总计 05:00", align = "center", y = 106,
  text_color = C.dim, font = f_ui,
})

-- Preset row -----------------------------------------------------

local PW, PH, GAP = 150, 50, 14
local row_w = #PRESETS * PW + (#PRESETS - 1) * GAP
local row_x = PX + (PANEL_W - row_w) // 2
local row_y = PY + 358
local preset_btns = {}

lvgl.label(scr, {
  text = "预设", x = row_x, y = row_y - 32,
  text_color = C.dim, font = f_ui,
})

for i, sec in ipairs(PRESETS) do
  local b = lvgl.button(scr, {
    text = preset_name(sec),
    x = row_x + (i - 1) * (PW + GAP), y = row_y, w = PW, h = PH,
    bg_color = C.btn, text_color = C.muted, font = f_ui,
    border_color = C.btn_edge, border_width = 2, radius = 14, pad = 0,
  })
  b:clear_flag("SCROLLABLE")
  preset_btns[i] = b
  b:on("clicked", function()
    state = "idle"
    PICK = i
    total = PRESETS[i]
    remaining = total
    finished_at = 0
    blink_on = false
    render()
  end)
end

-- Control buttons ------------------------------------------------

local CY, CH = PY + PANEL_H - 56, 48
local QW, START_W, CGAP = 120, 200, 20
local row_total = QW + CGAP + START_W + CGAP + QW
local QX = (W - row_total) // 2

local btn_prev = lvgl.button(scr, {
  text = "-", x = QX, y = CY, w = QW, h = CH,
  bg_color = C.btn, text_color = C.text, font = f_ui,
  border_color = C.btn_edge, border_width = 2, radius = 14, pad = 0,
})
btn_prev:clear_flag("SCROLLABLE")

local btn_next = lvgl.button(scr, {
  text = "+", x = QX + QW + CGAP + START_W + CGAP, y = CY,
  w = QW, h = CH,
  bg_color = C.btn, text_color = C.text, font = f_ui,
  border_color = C.btn_edge, border_width = 2, radius = 14, pad = 0,
})
btn_next:clear_flag("SCROLLABLE")

local btn_start = lvgl.button(scr, {
  text = "开始", x = QX + QW + CGAP, y = CY, w = START_W, h = CH,
  bg_color = C.accent, text_color = C.bg, font = f_ui,
  border_color = C.btn_edge, border_width = 2, radius = 14, pad = 0,
})
btn_start:clear_flag("SCROLLABLE")

btn_prev:on("clicked", function()
  local i = ((PICK - 2) % #PRESETS) + 1
  PICK = i
  state = "idle"
  total = PRESETS[i]
  remaining = total
  finished_at = 0
  blink_on = false
  render()
end)

btn_next:on("clicked", function()
  local i = (PICK % #PRESETS) + 1
  PICK = i
  state = "idle"
  total = PRESETS[i]
  remaining = total
  finished_at = 0
  blink_on = false
  render()
end)

local function start()
  if remaining <= 0 then
    remaining = total
  end
  state = "running"
  deadline = system.millis() + remaining * 1000
  finished_at = 0
  blink_on = false
  render()
end

btn_start:on("clicked", function()
  if state == "running" then
    state = "paused"
    render()
  elseif state == "paused" then
    start()
  elseif state == "done" then
    state = "idle"
    remaining = total
    finished_at = 0
    blink_on = false
    render()
  else
    start()
  end
end)

-- Render ---------------------------------------------------------

function render()
  local ratio = 0
  if total > 0 then
    ratio = math.max(0, math.min(1, remaining / total))
  end
  ring:set_value(math.floor(ratio * 1000 + 0.5))

  local color = C.ring_fg
  if state == "done" then
    color = C.ring_done
  elseif total > 0 and remaining > 0 and remaining <= total * 0.15 then
    color = C.ring_warn
  end
  ring:set_style({ line_color = color })

  if state == "done" then
    digits:set_style({ text_color = blink_on and C.ring_done or C.dim })
  else
    digits:set_style({ text_color = C.text })
  end
  digits:set_text(fmt(remaining))
  total_lbl:set_text("总计 " .. fmt(total))

  local st, h = "就绪", "准备开始"
  if state == "running" then
    st, h = "运行中", "正在倒计时"
  elseif state == "paused" then
    st, h = "已暂停", "点击继续"
  elseif state == "done" then
    st, h = "时间到", "时间到 · 点击重置"
  end
  state_lbl:set_text(st)
  hint:set_text(h)

  if state == "running" then
    btn_start:set_text("暂停")
    btn_start:set_style({ bg_color = C.gold, text_color = C.bg })
  elseif state == "paused" then
    btn_start:set_text("继续")
    btn_start:set_style({ bg_color = C.ring_fg, text_color = C.bg })
  elseif state == "done" then
    btn_start:set_text("重置")
    btn_start:set_style({ bg_color = C.ring_done, text_color = C.bg })
  else
    btn_start:set_text("开始")
    btn_start:set_style({ bg_color = C.accent, text_color = C.bg })
  end

  for i, b in ipairs(preset_btns) do
    if i == PICK then
      b:set_style({ bg_color = C.panel_2, text_color = C.accent, border_color = C.accent })
    else
      b:set_style({ bg_color = C.btn, text_color = C.muted, border_color = C.btn_edge })
    end
  end
end

-- Main -----------------------------------------------------------

render()
scr:load()

while not claw.abort_requested() do
  local now = system.millis()

  if state == "running" then
    local left = math.ceil((deadline - now) / 1000)
    if left < 0 then left = 0 end
    if left ~= remaining then
      remaining = left
      render()
    end
    if left <= 0 then
      state = "done"
      remaining = 0
      finished_at = now
      blink_on = true
      render()
    end
  elseif state == "done" then
    local on = (math.floor((now - finished_at) / 400) % 2) == 0
    if on ~= blink_on then
      blink_on = on
      render()
    end
  end

  lvgl.process_events(20)
end

-- Let the LVGL runtime delete the screen/labels before it releases the TTF
-- records. Deleting fonts first leaves dangling font pointers in live labels
-- and crashes during cleanup (Instruction access fault).
lvgl.deinit()
