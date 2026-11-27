-- 霓虹老虎机：3 轴 x 3 行经典玩法，转轴可点击提前停。
-- 纯 Lua + canvas：每帧约 30 次 C 侧图元操作，30-50 fps。
-- 中奖: OGG 音效 + 金币粒子 + claw.speak_async 语音播报（可关）。

local lvgl = require("lvgl")
local system = require("system")
local claw = require("claw")

local app_src = debug.getinfo(1, "S").source or ""
if app_src:sub(1, 1) == "@" then
  app_src = app_src:sub(2)
end
local app_dir = app_src:match("^(.*)/scripts/[^/]+$") or
                "/fatfs/claw/data/apps/slot_machine"
local AS = app_dir .. "/assets"

local ok_img, image = pcall(require, "image")
local ok_audio, audio = pcall(require, "audio")

-- ------------------------------------------------------------ palette

local C = {
  bg       = 0x05070d,
  col_bg   = 0x0b1220,
  col_edge = 0x1d2a44,
  gold     = 0xffd766,
  gold_dim = 0x8a6f2e,
  accent   = 0x3fe0c5,
  red      = 0xff5f6d,
  muted    = 0x7c8aa8,
  text     = 0xeef3ff,
  btn      = 0x121a2c,
  btn_edge = 0x243453,
}

-- ------------------------------------------------------------ constants

local SYMS  = { "seven", "bar", "diamond", "coin", "star", "cherry" }
local COUNT = { seven = 2, bar = 3, diamond = 4, coin = 4, star = 5, cherry = 6 }
local PAY   = { seven = 50, bar = 20, diamond = 12, coin = 8, star = 5, cherry = 3 }
local FALLBACK_COLOR = {
  seven = 0xffd766, bar = 0xff5f6d, diamond = 0x40beeb,
  coin = 0xeab348, star = 0xb07cff, cherry = 0xff4d6d,
}

local STRIP = 24
local REEL_X, REEL_Y, REEL_W, REEL_H = 32, 84, 576, 448
local COL_W, COL_GAP = 176, 24
local SYM_SZ = 128
local ROW_H = 144                     -- 128 + 16 gap
local SAVE = "slot_save.json"

local BETS = { 1, 2, 5, 10 }
local bet_index = 1

-- ------------------------------------------------------------ fonts

lvgl.init({ buffer_lines = 40, tick_ms = 5, task_period_ms = 10 })

local function load_font(px)
  local ok, f = pcall(lvgl.font_load, AS .. "/ui_cn.ttf",
                      { size = px, cache_size = 64 })
  return ok and f or nil
end

local f_ui    = load_font(20)
local f_title = load_font(28)
local f_big   = load_font(46)
if f_ui then
  pcall(lvgl.set_default_font, f_ui)
end

-- ------------------------------------------------------------ save data

local save = { balance = 500, best = 500, sfx = true, voice = true, auto = false }

local function persist()
  pcall(claw.file.write, SAVE, claw.json.encode({
    balance = save.balance, best = save.best,
    sfx = save.sfx, voice = save.voice, auto = save.auto,
  }))
end

do
  local ok, raw = pcall(claw.file.read, SAVE)
  if ok and raw and raw ~= "" then
    local ok2, t = pcall(claw.json.decode, raw)
    if ok2 and type(t) == "table" then
      save.balance = tonumber(t.balance) or save.balance
      save.best = tonumber(t.best) or save.best
      if t.sfx ~= nil then save.sfx = t.sfx and true or false end
      if t.voice ~= nil then save.voice = t.voice and true or false end
      if t.auto ~= nil then save.auto = t.auto and true or false end
    end
  end
end
save.balance = math.max(0, math.floor(save.balance))
save.best = math.max(save.best, save.balance)

-- ------------------------------------------------------------ screen & HUD

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#05070d", pad = 0 })
scr:clear_flag("SCROLLABLE")

local function mk_label(x, y, w, text, color, font)
  local l = lvgl.label(scr, { x = x, y = y, w = w, text = text })
  l:set_style({ text_color = color })
  if font then
    l:set_style({ text_font = font })
  end
  return l
end

local title_lbl = mk_label(24, 16, 400, "霓虹老虎机", C.gold, f_title)
local bal_lbl = mk_label(REEL_X, 16, REEL_W, "", C.text, f_title)

local best_lbl = mk_label(REEL_X, 54, REEL_W, "", C.muted, f_ui)

-- win banner (over the reels)
local win_lbl = mk_label(REEL_X, 240, REEL_W, "", C.gold, f_big)
pcall(win_lbl.set_style, win_lbl, { text_align = "center" })
win_lbl:add_flag("HIDDEN")

-- ------------------------------------------------------------ symbols

local icons = {}
if ok_img and image then
  for _, s in ipairs(SYMS) do
    local ok, f = pcall(image.load_file, AS .. "/symbols/" .. s .. ".jpg")
    if ok and f then
      icons[s] = f
    end
  end
end

-- ------------------------------------------------------------ canvas

local canvas <close> = ok_img and image and image.new(REEL_W, REEL_H, image.RGB565) or nil

local reel_img = canvas and lvgl.image(scr, { x = REEL_X, y = REEL_Y, w = REEL_W, h = REEL_H, pad = 0 }) or nil
if reel_img then
  reel_img:clear_flag("CLICKABLE")
end
if not canvas then

  mk_label(REEL_X, 200, REEL_W, "image 模块不可用", C.red, f_ui)
end

-- ------------------------------------------------------------ game state

local reels = {}
local state = "idle"          -- idle | spinning | settle | auto_wait
local settle_at = 0
local next_auto = 0
local stop_requested = { false, false, false }
local win_lines = {}
local win_gain = 0
local particles = {}
local last = system.millis()
local quit = false

-- created in the right-panel section below; declared here so update_hud()
-- (defined above the widgets) sees them as upvalues, not globals
local bet_btns, snd_btn, vce_btn, aut_btn

math.randomseed(system.millis() % 100000)

local function new_strip()
  local pool = {}
  for _, s in ipairs(SYMS) do
    for _ = 1, COUNT[s] do
      pool[#pool + 1] = s
    end
  end
  for i = #pool, 2, -1 do
    local j = math.random(i)
    pool[i], pool[j] = pool[j], pool[i]
  end
  return pool
end

for _ = 1, 3 do
  reels[#reels + 1] = {
    strip = new_strip(), pos = 0, speed = 0, phase = "idle",
    stop_at = 0, start_pos = 0, target_pos = 0, stop_start_ms = 0, stop_dur = 0,
  }
end

local function play(name)
  if save.sfx and ok_audio and audio and audio.play then
    pcall(audio.play, name)
  end
end

-- ------------------------------------------------------------ spin engine

local function update_hud()
  bal_lbl:set_text("余额 " .. save.balance)
  best_lbl:set_text("最高 " .. save.best)
  for i, b in ipairs(bet_btns) do
    local sel = (i == bet_index)
    b:set_style({ bg_color = sel and C.accent or C.btn,
                  text_color = sel and 0x052e28 or C.text,
                  border_color = sel and 0x6ff2dd or C.btn_edge })
  end
  snd_btn:set_text(save.sfx and "开" or "关")
  vce_btn:set_text(save.voice and "开" or "关")
  aut_btn:set_text(save.auto and "开" or "关")
  snd_btn:set_style({ bg_color = save.sfx and C.accent or C.btn,
                      text_color = save.sfx and 0x052e28 or C.text })
  vce_btn:set_style({ bg_color = save.voice and C.accent or C.btn,
                      text_color = save.voice and 0x052e28 or C.text })
  aut_btn:set_style({ bg_color = save.auto and C.accent or C.btn,
                      text_color = save.auto and 0x052e28 or C.text })
end

local function begin_stop(i)
  local r = reels[i]
  r.phase = "stopping"
  r.stop_start_ms = system.millis()
  local target_index = math.random(STRIP)
  local frac = r.pos - math.floor(r.pos)
  local cur = math.floor(r.pos) % STRIP + 1
  local delta = (target_index - cur) % STRIP
  local extra = STRIP * (1 + math.random(2))
  r.start_pos = r.pos
  r.target_pos = r.pos + delta + extra - frac
  r.stop_dur = 650 + delta * 22
end

local function request_stop(i)
  if state == "spinning" and reels[i].phase == "spinning" then
    stop_requested[i] = true
  end
end

local function start_spin()
  if state ~= "idle" and state ~= "auto_wait" then
    return
  end
  if save.balance < BETS[bet_index] then
    state = "idle"  -- leave auto_wait too, or the loop would busy-retry
    if save.balance <= 0 then
      save.balance = 100
      if save.balance > save.best then save.best = save.balance end
      persist()
      update_hud()
      if save.voice then
        pcall(claw.speak_async, "破产了，再送你一百金币")
      end
    end
    return
  end
  save.balance = save.balance - BETS[bet_index]
  if save.balance > save.best then save.best = save.balance end
  persist()
  update_hud()

  win_gain = 0
  win_lines = {}
  particles = {}
  win_lbl:add_flag("HIDDEN")
  play("spin")

  for i = 1, 3 do
    local r = reels[i]
    r.strip = new_strip()
    r.phase = "spinning"
    r.speed = 0
    r.stop_at = system.millis() + 1200 + math.random(1400)
    stop_requested[i] = false
  end
  state = "spinning"
end

local function sym_at(i, row)
  local r = reels[i]
  local idx = (math.floor(r.pos) + row) % STRIP + 1
  return r.strip[idx]
end

local function spawn_particles()
  local seed = {}
  for _, line in ipairs(win_lines) do
    for _, c in ipairs(line.cells) do
      seed[#seed + 1] = c
    end
  end
  if #seed == 0 then
    return
  end
  for n = 1, 42 do
    local c = seed[(n % #seed) + 1]
    local col, row = c[1], c[2]
    particles[#particles + 1] = {
      x = REEL_X + (col - 1) * (COL_W + COL_GAP) + SYM_SZ // 2 + math.random(-36, 36),
      y = REEL_Y + 16 + row * ROW_H + SYM_SZ // 2,
      vx = (math.random(200) - 100) / 100,
      vy = -(math.random(60, 170)) / 100,
      life = 0,
      max = 1000 + math.random(1400),
    }
  end
end

local function show_win(gain, jackpot)
  win_lbl:set_text(jackpot and ("大满贯 +" .. gain) or ("中奖 +" .. gain))
  win_lbl:clear_flag("HIDDEN")
end

local function evaluate()
  local lines = {
    { name = "row1",  cells = { { 1, 0 }, { 2, 0 }, { 3, 0 } } },
    { name = "row2",  cells = { { 1, 1 }, { 2, 1 }, { 3, 1 } } },
    { name = "row3",  cells = { { 1, 2 }, { 2, 2 }, { 3, 2 } } },
    { name = "diag1", cells = { { 1, 0 }, { 2, 1 }, { 3, 2 } } },
    { name = "diag2", cells = { { 1, 2 }, { 2, 1 }, { 3, 0 } } },
  }
  local gain = 0
  local hits = {}
  local jackpot = false
  for _, line in ipairs(lines) do
    local s1 = sym_at(line.cells[1][1], line.cells[1][2])
    local s2 = sym_at(line.cells[2][1], line.cells[2][2])
    local s3 = sym_at(line.cells[3][1], line.cells[3][2])
    if s1 == s2 and s2 == s3 then
      gain = gain + BETS[bet_index] * PAY[s1]
      hits[#hits + 1] = line
      if s1 == "seven" then
        jackpot = true
      end
    elseif s1 == s2 or s2 == s3 then
      gain = gain + BETS[bet_index]
    end
  end

  win_lines = hits
  win_gain = gain
  save.balance = save.balance + gain
  if save.balance > save.best then
    save.best = save.balance
  end
  persist()
  update_hud()
  settle_at = system.millis()
  state = "settle"

  if gain > 0 then
    play(jackpot and "jackpot" or "win")
    spawn_particles()
    show_win(gain, jackpot)
    if save.voice then
      if jackpot then
        pcall(claw.speak_async, "七！七！七！大满贯！")
      elseif gain >= BETS[bet_index] * 10 then
        pcall(claw.speak_async, "恭喜中奖 " .. gain .. " 金币")
      end
    end
  end
end

local function update_reels(now, dt)
  local all_stopped = true
  for i = 1, 3 do
    local r = reels[i]
    if r.phase == "spinning" then
      all_stopped = false
      r.speed = math.min(r.speed + 40 * dt, 24)
      r.pos = r.pos + r.speed * dt
      if (stop_requested[i] and r.speed > 14) or now >= r.stop_at then
        begin_stop(i)
      end
    elseif r.phase == "stopping" then
      all_stopped = false
      local p = (now - r.stop_start_ms) / r.stop_dur
      if p >= 1 then
        r.pos = r.target_pos
        r.phase = "stopped"
        play("stop")
      else
        local e = 1 - (1 - p) * (1 - p) * (1 - p)
        r.pos = r.start_pos + (r.target_pos - r.start_pos) * e
      end
    end
  end
  if state == "spinning" and all_stopped then
    evaluate()
  end
end

-- ------------------------------------------------------------ drawing

local function draw()
  if not canvas then
    return
  end
  canvas:fill("#05070d")

  for i = 1, 3 do
    local x = (i - 1) * (COL_W + COL_GAP)
    canvas:fill_rect(x, 0, COL_W, REEL_H, C.col_bg)
    canvas:fill_rect(x, 0, COL_W, 2, C.col_edge)
    canvas:fill_rect(x, REEL_H - 2, COL_W, 2, C.col_edge)
  end

  -- symbols: rows -1..2 so the scrolling gap never shows column bg at the top
  for i = 1, 3 do
    local r = reels[i]
    local x = (i - 1) * (COL_W + COL_GAP) + (COL_W - SYM_SZ) // 2
    local frac = r.pos - math.floor(r.pos)
    local off = math.floor(frac * ROW_H + 0.5)
    for row = -1, 2 do
      local idx = (math.floor(r.pos) + row) % STRIP + 1
      local sym = r.strip[idx]
      local y = 16 + row * ROW_H + off
      if y > -SYM_SZ and y < REEL_H then
        local icon = icons[sym]
        if icon then
          canvas:blit(icon, x, y, SYM_SZ)
        else
          canvas:fill_rect(x + 10, y + 10, SYM_SZ - 20, SYM_SZ - 20,
                           FALLBACK_COLOR[sym])
        end
      end
    end
  end

  -- separators
  for row = 1, 2 do
    canvas:fill_rect(0, 16 + row * ROW_H - 8, REEL_W, 2, 0x1a2740)
  end

  -- winning line highlights
  for _, line in ipairs(win_lines) do
    local first = line.cells[1]
    local last_c = line.cells[#line.cells]
    local x1 = (first[1] - 1) * (COL_W + COL_GAP) + 8
    local x2 = (last_c[1] - 1) * (COL_W + COL_GAP) + COL_W - 8
    local y1 = 16 + first[2] * ROW_H + SYM_SZ // 2
    local y2 = 16 + last_c[2] * ROW_H + SYM_SZ // 2
    canvas:line(x1, y1, x2, y2, 0xffd766, 4)
  end

  -- particles
  for _, p in ipairs(particles) do
    local k = p.life / p.max
    local s = 4 + math.floor(k * 4)
    canvas:fill_rect(math.floor(p.x), math.floor(p.y), s, s, C.gold)
  end

  if reel_img then
    reel_img:set_frame(canvas)
  end
end

local function update_particles(dt)
  local alive = {}
  for _, p in ipairs(particles) do
    p.life = p.life + dt * 1000
    if p.life < p.max then
      p.vy = p.vy + 0.0016 * dt * 1000
      p.x = p.x + p.vx * dt * 1000
      p.y = p.y + p.vy * dt * 1000
      alive[#alive + 1] = p
    end
  end
  particles = alive
end

-- ------------------------------------------------------------ right panel

local bet_label = mk_label(640, 100, 200, "下注", C.muted, f_ui)
bet_btns = {}
for i, v in ipairs(BETS) do
  local b = lvgl.button(scr, {
    x = 636 + (i - 1) * 90, y = 128, w = 82, h = 52, text = tostring(v),
    radius = 12,
  })
  b:set_style({ border_width = 1 })
  b:on("clicked", function()
    bet_index = i
    update_hud()
  end)
  bet_btns[i] = b
end


local spin_btn = lvgl.button(scr, { x = 636, y = 216, w = 356, h = 118, text = "开始", radius = 20 })
spin_btn:set_style({ border_width = 0 })
spin_btn:set_style({ text_font = f_title })
spin_btn:on("clicked", start_spin)

-- tap zones over each reel column for early stop
for i = 1, 3 do
  local z = lvgl.button(scr, {
    x = REEL_X + (i - 1) * (COL_W + COL_GAP), y = REEL_Y,
    w = COL_W, h = REEL_H, radius = 0,
  })
  z:set_style({ bg_opa = 0, bg_color = "#000000", border_width = 0, shadow_width = 0 })
  local idx = i
  z:on("pressed", function()
    request_stop(idx)
  end)
end

-- toggles
do
  local function toggle_row(y, name, getter, setter)
    local lbl = mk_label(640, y + 10, 120, name, C.text, f_ui)
    local b = lvgl.button(scr, { x = 900, y = y, w = 92, h = 44, radius = 10 })
    b:set_style({ border_width = 1 })
    b:on("clicked", function()
      setter(not getter())
      persist()
      update_hud()
    end)
    return b
  end
  snd_btn = toggle_row(368, "音效", function() return save.sfx end,
                      function(v) save.sfx = v end)
  vce_btn = toggle_row(424, "语音", function() return save.voice end,
                      function(v) save.voice = v end)
  aut_btn = toggle_row(480, "自动", function() return save.auto end,
                      function(v) save.auto = v end)
end


local quit_btn = lvgl.button(scr, { x = 636, y = 540, w = 356, h = 48, text = "退出", radius = 12 })
quit_btn:set_style({ border_width = 1 })
quit_btn:on("clicked", function()
  quit = true
end)

-- ------------------------------------------------------------ main loop

update_hud()
draw()

scr:load()

while not claw.abort_requested() and not quit do
  local now = system.millis()
  local dt = math.min(now - last, 60) / 1000
  last = now

  if state == "spinning" then
    update_reels(now, dt)
    draw()
  elseif state == "settle" then
    update_particles(dt)
    draw()
    if now - settle_at > 2600 then
      win_lbl:add_flag("HIDDEN")
      particles = {}
      if save.auto and save.balance >= BETS[bet_index] then
        state = "auto_wait"
        next_auto = now + 1200
      else
        state = "idle"
      end
      draw()
    end
  elseif state == "auto_wait" and now >= next_auto then
    start_spin()
  end

  lvgl.process_events(20)
end

-- Let the LVGL runtime delete the screen before releasing TTF records.
lvgl.deinit()
