-- Answer Book for the 1024x600 ESP32-P4 panel.
-- A closed book opens with a page-flip animation and reveals a random answer.
-- Assets: assets/bg.jpg (1024x600), assets/answer_cn.ttf, assets/sfx/flip.ogg.

local lvgl = require("lvgl")
local system = require("system")

local app_src = debug.getinfo(1, "S").source or ""
if app_src:sub(1, 1) == "@" then
  app_src = app_src:sub(2)
end
local app_dir = app_src:match("^(.*)/scripts/[^/]+$") or
                "/fatfs/claw/data/apps/answer_book"

local LOG_PATH = "/fatfs/claw/data/answer_log.txt"
local function log(msg)
  local ok, f = pcall(io.open, LOG_PATH, "a")
  if ok and f then
    pcall(f.write, f, tostring(msg), "\n")
    pcall(f.close, f)
  end
end
log("script start")

local ok_audio, audio = pcall(require, "audio")
local ok_image, image = pcall(require, "image")

local W, H = 1024, 600

local C = {
  bg       = "#070a12",
  ink      = "#2a2119",
  paper    = "#f2e6c8",
  paper_2  = "#e9d9b7",
  gold     = "#d8b45a",
  gold_dim = "#9d7f3f",
  cover    = "#3a1b2e",
  cover_2  = "#25101d",
  ribbon   = "#8c2f39",
  muted    = "#8d7951",
}

local ANSWERS = {
  "是的，时机正好。",
  "再等等，答案会出现。",
  "先看清你真正的问题。",
  "值得一试，别犹豫。",
  "答案一直在你心里。",
  "现在还不是时候。",
  "换个角度，再问一次。",
  "会有惊喜，方式不同。",
  "放下执念，顺其自然。",
  "去做吧，别回头。",
  "别想了，先行动。",
  "先照顾好自己。",
  "相信第一直觉。",
  "比你想象的更简单。",
  "暂时搁置，明天再说。",
}

local function sfx(name)
  if ok_audio and audio and audio.play then
    pcall(audio.play, name)
  end
end

local function font_or(font, fallback)
  return font or fallback
end

local function clamp01(t)
  if t < 0 then return 0 end
  if t > 1 then return 1 end
  return t
end

local function smooth(t)
  t = clamp01(t)
  return t * t * (3 - 2 * t)
end

lvgl.init({ buffer_lines = 40, tick_ms = 5, task_period_ms = 10 })
math.randomseed(system.millis())

local function load_font(px, cache)
  local ok, font = pcall(lvgl.font_load,
                         app_dir .. "/assets/answer_cn.ttf",
                         { size = px, cache_size = cache })
  if ok and font then
    return font
  end
  return nil
end

local f_small = load_font(16, 64)
local f_ui = load_font(20, 128)
local f_title = load_font(34, 64)
local f_answer = load_font(30, 96)
local f_cover = load_font(64, 64)
if f_ui then
  lvgl.set_default_font(f_ui)
end

local scr = lvgl.create_screen()
scr:set_style({ bg_color = C.bg, pad = 0 })
scr:clear_flag("SCROLLABLE")

-- Background ------------------------------------------------------
local bg = lvgl.image(scr, { x = 0, y = 0, w = W, h = H, pad = 0 })
bg:clear_flag("CLICKABLE")
if ok_image and image then
  local ok, frame = pcall(image.load_file, app_dir .. "/assets/bg.jpg")
  if ok and frame then
    local ok2, rgb = pcall(image.convert, frame, image.RGB565)
    frame:release()
    if ok2 and rgb then
      local ok3, data = pcall(rgb.data, rgb)
      rgb:release()
      if ok3 and data then
        pcall(bg.set_raw_rgb565, bg, W, H, data)
      end
    end
  end
end

-- Header ----------------------------------------------------------
lvgl.label(scr, {
  text = "THE BOOK OF ANSWERS", align = "top_mid", y = 18,
  text_color = C.gold_dim, font = f_small,
})

-- Open book pages -------------------------------------------------
local shadow = lvgl.container(scr, {
  x = 192, y = 522, w = 640, h = 16,
  bg_color = "#000000", bg_opa = 120, radius = 8,
  border_width = 0, pad = 0,
})
shadow:clear_flag("SCROLLABLE")
shadow:set_style({ opa = 0 })

local pages = lvgl.container(scr, {
  x = 172, y = 78, w = 680, h = 448,
  bg_opa = 0, border_width = 0, pad = 0,
})
pages:clear_flag("SCROLLABLE")
pages:set_style({ opa = 0 })

local left_page = lvgl.container(pages, {
  x = 8, y = 6, w = 332, h = 432,
  bg_color = C.paper, radius = 10,
  border_color = C.gold, border_width = 2, pad = 0,
})
left_page:clear_flag("SCROLLABLE")

local right_page = lvgl.container(pages, {
  x = 340, y = 6, w = 332, h = 432,
  bg_color = C.paper_2, radius = 10,
  border_color = C.gold, border_width = 2, pad = 0,
})
right_page:clear_flag("SCROLLABLE")

local spine = lvgl.container(pages, {
  x = 332, y = 6, w = 8, h = 432,
  bg_color = C.cover_2, radius = 2,
  border_width = 0, pad = 0,
})
spine:clear_flag("SCROLLABLE")

local left_rule = lvgl.container(left_page, {
  x = 24, y = 64, w = 284, h = 1,
  bg_color = C.gold_dim, border_width = 0, pad = 0,
})
left_rule:clear_flag("SCROLLABLE")

lvgl.label(left_page, {
  text = "答 案", x = 24, y = 24,
  text_color = C.muted, font = f_ui,
})
lvgl.label(left_page, {
  text = "“", x = 16, y = 84,
  text_color = C.gold, font = f_cover,
})
lvgl.label(left_page, {
  text = "心诚则灵", align = "bottom_left", x = 24, y = -32,
  text_color = C.muted, font = f_small,
})

local answer_lbl = lvgl.label(right_page, {
  text = "", align = "center", y = 0,
  text_color = C.ink, font = f_answer,
})
answer_lbl:add_flag("HIDDEN")
answer_lbl:set_style({ opa = 0 })

-- Page-flip layer ------------------------------------------------
local flip_page = lvgl.container(pages, {
  x = 340, y = 6, w = 332, h = 432,
  bg_color = C.paper, radius = 10,
  border_color = C.gold, border_width = 2, pad = 0,
})
flip_page:clear_flag("SCROLLABLE")
flip_page:add_flag("HIDDEN")

local flip_edge = lvgl.container(pages, {
  x = 668, y = 6, w = 8, h = 432,
  bg_color = "#3a2c1c", bg_opa = 110, radius = 3,
  border_width = 0, pad = 0,
})
flip_edge:clear_flag("SCROLLABLE")
flip_edge:add_flag("HIDDEN")

-- Click target over the open pages --------------------------------
local hit = lvgl.container(scr, {
  x = 180, y = 84, w = 664, h = 432,
  bg_opa = 0, border_width = 0, pad = 0,
})
hit:clear_flag("SCROLLABLE")
hit:add_flag("CLICKABLE")
hit:add_flag("HIDDEN")

-- Closed cover ----------------------------------------------------
local cover = lvgl.container(scr, {
  x = 352, y = 70, w = 320, h = 448,
  bg_color = C.cover, radius = 18,
  border_color = C.gold, border_width = 3, pad = 0,
})
cover:clear_flag("SCROLLABLE")
cover:add_flag("CLICKABLE")

local inner = lvgl.container(cover, {
  x = 12, y = 12, w = 296, h = 424,
  bg_opa = 0, border_color = C.gold_dim, border_width = 1,
  radius = 14, pad = 0,
})
inner:clear_flag("SCROLLABLE")
-- Children cover the whole book face; keep them non-clickable so taps fall
-- through to the cover's clicked handler.
inner:clear_flag("CLICKABLE")

local cover_spine = lvgl.container(cover, {
  x = 0, y = 0, w = 12, h = 448,
  bg_color = C.cover_2, radius = 12, border_width = 0, pad = 0,
})
cover_spine:clear_flag("SCROLLABLE")
cover_spine:clear_flag("CLICKABLE")

local ribbon = lvgl.container(cover, {
  x = 250, y = 0, w = 16, h = 64,
  bg_color = C.ribbon, radius = 3, border_width = 0, pad = 0,
})
ribbon:clear_flag("SCROLLABLE")
ribbon:clear_flag("CLICKABLE")

-- Cover wordmark: no decorative arc; a quiet gold rule and a square seal.
local cover_title = lvgl.label(cover, {
  text = "答案之书", align = "top_mid", y = 74,
  text_color = C.gold, font = f_title,
})
cover_title:clear_flag("CLICKABLE")

local cover_rule = lvgl.container(cover, {
  x = 80, y = 130, w = 160, h = 1,
  bg_color = C.gold_dim, border_width = 0, pad = 0,
})
cover_rule:clear_flag("SCROLLABLE")
cover_rule:clear_flag("CLICKABLE")

local cover_sub = lvgl.label(cover, {
  text = "轻触翻开", align = "top_mid", y = 144,
  text_color = C.muted, font = f_ui,
})
cover_sub:clear_flag("CLICKABLE")

local cover_seal = lvgl.container(cover, {
  x = 108, y = 208, w = 104, h = 104,
  bg_opa = 0, border_color = C.gold, border_width = 2,
  radius = 16, pad = 0,
})
cover_seal:clear_flag("SCROLLABLE")
cover_seal:clear_flag("CLICKABLE")

local cover_seal_inner = lvgl.container(cover_seal, {
  x = 6, y = 6, w = 88, h = 88,
  bg_opa = 0, border_color = C.gold_dim, border_width = 1,
  radius = 12, pad = 0,
})
cover_seal_inner:clear_flag("SCROLLABLE")
cover_seal_inner:clear_flag("CLICKABLE")

local cover_mark = lvgl.label(cover_seal, {
  text = "答", align = "center", y = 0,
  text_color = C.gold, font = f_cover,
})
cover_mark:clear_flag("CLICKABLE")
local cover_footer = lvgl.label(cover, {
  text = "问 心 · 翻 页", align = "bottom_mid", y = -38,
  text_color = C.gold_dim, font = f_small,
})
cover_footer:clear_flag("CLICKABLE")

-- A transparent click target over the whole cover. Child widgets can swallow
-- clicks on some builds; this overlay guarantees the book opens.
local cover_hit = lvgl.container(scr, {
  x = 352, y = 70, w = 320, h = 448,
  bg_opa = 0, border_width = 0, pad = 0,
})
cover_hit:clear_flag("SCROLLABLE")
cover_hit:add_flag("CLICKABLE")

-- Bottom hint / re-ask -------------------------------------------
local hint = lvgl.label(scr, {
  text = "轻触书本，翻开答案", align = "bottom_mid", y = -6,
  text_color = C.gold_dim, font = f_small,
})


local btn_again = lvgl.button(scr, {
  text = "再问一次", x = 412, y = 526, w = 200, h = 44,
  bg_color = C.gold, text_color = C.cover_2, font = f_ui,
  border_color = C.gold_dim, border_width = 1, radius = 14, pad = 0,
})
btn_again:clear_flag("SCROLLABLE")
btn_again:add_flag("HIDDEN")
btn_again:set_style({ opa = 0 })

-- ------------------------------------------------------------- state
local state = "closed"
local open_t0 = 0
local flip_t0 = 0
local answer_t0 = 0
local answer = ""
local last_answer = nil

local function choose_answer()
  if #ANSWERS == 0 then return "顺其自然。" end
  local pick = ANSWERS[math.random(#ANSWERS)]
  if #ANSWERS > 1 then
    local guard = 0
    while pick == last_answer and guard < 8 do
      pick = ANSWERS[math.random(#ANSWERS)]
      guard = guard + 1
    end
  end
  last_answer = pick
  return pick
end

local function start_flip(now)
  log("start_flip")
  answer = choose_answer()
  flip_t0 = now
  state = "flipping"
  answer_lbl:set_text("")
  answer_lbl:add_flag("HIDDEN")
  answer_lbl:set_style({ opa = 0 })
  btn_again:add_flag("HIDDEN")
  btn_again:set_style({ opa = 0 })
  flip_page:clear_flag("HIDDEN")
  flip_page:set_pos(340, 6)
  flip_page:set_size(332, 432)
  flip_edge:clear_flag("HIDDEN")
  flip_edge:set_pos(668, 6)
  hint:set_text("正在翻页…")
  sfx("flip")
end

local function finish_flip(now)
  log("finish_flip answer=" .. tostring(answer))
  flip_page:add_flag("HIDDEN")
  flip_edge:add_flag("HIDDEN")
  answer_lbl:set_text(answer)
  answer_lbl:clear_flag("HIDDEN")
  answer_lbl:set_style({ opa = 0 })
  btn_again:clear_flag("HIDDEN")
  btn_again:set_style({ opa = 0 })
  hint:set_text("轻触书页，再问一次")
  answer_t0 = now
  state = "answer"
end

local function book_clicked()
  log("click state=" .. state)
  local ok, err = pcall(function()
    local now = system.millis()
    if state == "closed" then
      state = "opening"
      open_t0 = now
      cover_hit:add_flag("HIDDEN")
      hint:set_text("正在翻开…")
      sfx("flip")
    elseif state == "answer" then
      start_flip(now)
    end
  end)
  if not ok then
    log("click error: " .. tostring(err))
  end
end

local function reask_clicked()
  if state == "answer" then
    start_flip(system.millis())
  end
end

cover:on("clicked", book_clicked)
cover_hit:on("clicked", book_clicked)
hit:on("clicked", book_clicked)
btn_again:on("clicked", reask_clicked)

-- Load the app screen into the exclusive display session before entering the
-- event loop. Without this the objects exist but the previous screen stays
-- visible (only the system background is shown).
scr:load()

-- ------------------------------------------------------------- loop
while not claw.abort_requested() do
  local now = system.millis()

  local ok, err = pcall(function()
    if state == "closed" then
      local bob = math.sin(now / 680.0) * 4
      cover:set_pos(352, math.floor(70 + bob))
    elseif state == "opening" then
      local t = (now - open_t0) / 680.0
      local e = smooth(t)
      local cover_w = math.max(1, math.floor(320 * (1 - e)))
      cover:set_pos(math.floor(352 - 180 * e), math.floor(70 - 18 * e))
      cover:set_size(cover_w, 448)
      cover:set_style({ opa = math.floor(255 * (1 - e)) })
      pages:set_style({ opa = math.floor(255 * e) })
      shadow:set_style({ opa = math.floor(120 * e) })
      if t >= 1 then
        log("opening_done")
        cover:add_flag("HIDDEN")
        hit:clear_flag("HIDDEN")
        pages:set_style({ opa = 255 })
        shadow:set_style({ opa = 120 })
        start_flip(now)
      end
    elseif state == "flipping" then
      local t = (now - flip_t0) / 540.0
      local e = smooth(t)
      local w = math.max(1, math.floor(332 * (1 - e)))
      flip_page:set_pos(340, 6)
      flip_page:set_size(w, 432)
      flip_edge:set_pos(340 + w - 4, 6)
      flip_edge:set_size(8, 432)
      if t >= 1 then
        finish_flip(now)
      end
    elseif state == "answer" then
      local t = (now - answer_t0) / 380.0
      local e = smooth(t)
      local opa = math.floor(255 * e)
      answer_lbl:set_style({ opa = opa })
      btn_again:set_style({ opa = opa })
    end
  end)
  if not ok then
    log("loop error: " .. tostring(err))
    break
  end

  lvgl.process_events(20)
end

log("loop exit abort=" .. tostring(claw.abort_requested()))
lvgl.deinit()
