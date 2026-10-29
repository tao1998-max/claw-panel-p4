-- Blackjack 21 for the 1024x600 ESP32-P4 panel.
-- Uses only the on-device LVGL runtime and a small bundled TTF subset.

local lvgl = require("lvgl")
local system = require("system")
local ok_audio, audio = pcall(require, "audio")
if not ok_audio then
  audio = nil
end

local W = 1024
local CARD_W, CARD_H = 96, 140
local DECK_X, DECK_Y = 902, 74
local DEALER_Y, PLAYER_Y = 128, 372

local C = {
  bg          = "#051710",
  felt        = "#0a3025",
  gold        = "#d9b65a",
  gold_dim    = "#7c6630",
  cream       = "#f7f2e4",
  card_edge   = "#d9cfb8",
  red         = "#b3261e",
  black       = "#161616",
  back        = "#16285e",
  muted       = "#9bb3a8",
  win         = "#7ee2a8",
  lose        = "#ff8a7a",
  push        = "#e8d18a",
  btn         = "#1d4a3a",
  btn_text    = "#f4e9c8",
}

local app_src = debug.getinfo(1, "S").source or ""
if app_src:sub(1, 1) == "@" then app_src = app_src:sub(2) end
local app_dir = app_src:match("^(.*)/scripts/[^/]+$") or
                "/fatfs/claw/data/apps/blackjack"

local function sfx(name)
  if audio and audio.play then
    -- audio.play("deal") resolves <app>/assets/sfx/deal.ogg
    pcall(audio.play, name)
  end
end

lvgl.init({ buffer_lines = 40, tick_ms = 5, task_period_ms = 10 })

local function load_font(px, cache)
  local ok, font = pcall(lvgl.font_load, app_dir .. "/assets/game_cn.ttf",
                         { size = px, cache_size = cache })
  if ok and font then
    return font
  end
  return nil
end

local f_ui = load_font(18, 128)
local f_rank = load_font(28, 96)
local f_title = load_font(48, 64)
if f_ui then
  lvgl.set_default_font(f_ui)
end

local function styled(opts, font)
  if font then
    opts.font = font
  end
  return opts
end

local scr = lvgl.create_screen()
scr:set_style({ bg_color = C.bg, pad = 0 })

local frame = lvgl.container(scr, {
  x = 14, y = 12, w = 996, h = 576,
  bg_color = C.felt,
  radius = 28,
  border_color = C.gold,
  border_width = 2,
  pad = 0,
})
frame:clear_flag("SCROLLABLE")
lvgl.container(scr, {
  x = 24, y = 22, w = 976, h = 556,
  bg_opa = 0,
  border_color = C.gold_dim,
  border_width = 1,
  radius = 20,
  pad = 0,
})

lvgl.label(scr, styled({ text = "二 十 一 点", align = "top_mid", y = 16,
                         text_color = C.gold }, f_title))
lvgl.label(scr, styled({ text = "幸 运 2 1", align = "top_mid", y = 78,
                         text_color = C.muted }, f_ui))
lvgl.label(scr, styled({ text = "黑桃", x = 246, y = 44,
                         text_color = C.gold_dim }, f_ui))
lvgl.label(scr, styled({ text = "红桃", x = 700, y = 44,
                         text_color = C.red }, f_ui))

local score_lbl = lvgl.label(scr, styled({
  text = "胜 0   负 0   平 0", x = 40, y = 38, text_color = C.muted }, f_ui))

lvgl.line(scr, {
  points = { { x = 60, y = 102 }, { x = 964, y = 102 } },
  line_color = C.gold_dim,
  line_width = 1,
})

-- Decorative deck stack (the animated cards start from this spot).
for i = 3, 1, -1 do
  local back = lvgl.container(scr, {
    x = 900 + (i - 1) * 4, y = 72 + (i - 1) * 4,
    w = 64, h = 92,
    bg_color = C.back,
    radius = 8,
    border_color = C.gold_dim,
    border_width = 1,
    pad = 0,
  })
  back:clear_flag("SCROLLABLE")
end
lvgl.label(scr, styled({ text = "♠", align = "top_right", x = -120, y = 96,
                         text_color = C.gold }, f_rank))
local deck_lbl = lvgl.label(scr, styled({ text = "52", x = 920, y = 174,
                                          text_color = C.muted }, f_ui))

lvgl.label(scr, styled({ text = "庄家", x = 40, y = 112,
                         text_color = C.muted }, f_ui))
local dealer_score = lvgl.label(scr, styled({ text = "?", x = 40, y = 134,
                                              text_color = C.gold }, f_title))
lvgl.label(scr, styled({ text = "玩家", x = 40, y = 356,
                         text_color = C.muted }, f_ui))
local player_score = lvgl.label(scr, styled({ text = "0", x = 40, y = 378,
                                              text_color = C.cream }, f_title))

local message = lvgl.label(scr, styled({
  text = "开始发牌", align = "top_mid", y = 276,
  text_color = C.gold }, f_rank))
lvgl.label(scr, styled({
  text = "庄家 17 点停牌 · 黑杰克赔率 3:2",
  align = "top_mid", y = 312, text_color = C.muted }, f_ui))

local btn_deal, btn_hit, btn_stand

local function make_button(text, x, w, primary)
  local btn = lvgl.button(scr, styled({
    text = text, x = x, y = 526, w = w, h = 50,
    bg_color = primary and C.gold or C.btn,
    text_color = primary and C.bg or C.btn_text,
    border_color = C.gold_dim,
    border_width = 1,
    radius = 14,
    pad = 0,
  }, f_rank))
  btn:clear_flag("SCROLLABLE")
  return btn
end

btn_deal = make_button("发牌", 392, 240, true)
btn_hit = make_button("要牌", 300, 180, false)
btn_stand = make_button("停牌", 544, 180, false)

local SUITS = { "♠", "♥", "♦", "♣" }
local RANKS = { "A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K" }
local VALUES = { 11, 2, 3, 4, 5, 6, 7, 8, 9, 10, 10, 10, 10 }
local RED = { [2] = true, [3] = true }

local deck = {}
local anims = {}
local player, dealer = {}, {}
local state = "idle"
local wins, losses, pushes = 0, 0, 0
local dealer_revealed = false
local deal_ready_at = 0
local dealer_next_at = 0

local function new_deck()
  deck = {}
  for s = 1, 4 do
    for r = 1, 13 do
      deck[#deck + 1] = { rank = r, suit = s }
    end
  end
  for i = #deck, 2, -1 do
    local j = math.random(i)
    deck[i], deck[j] = deck[j], deck[i]
  end
end

local function hand_total(hand)
  local sum, aces = 0, 0
  for _, card in ipairs(hand) do
    sum = sum + card.value
    if card.value == 11 then
      aces = aces + 1
    end
  end
  while sum > 21 and aces > 0 do
    sum = sum - 10
    aces = aces - 1
  end
  return sum
end

local function make_card(rank_i, suit_i, face_up)
  local root = lvgl.container(scr, {
    x = DECK_X, y = DECK_Y, w = CARD_W, h = CARD_H,
    bg_opa = 0, border_width = 0, pad = 0,
  })
  root:clear_flag("SCROLLABLE")

  local back = lvgl.container(root, {
    w = CARD_W, h = CARD_H,
    bg_color = C.back,
    radius = 10,
    border_color = C.gold,
    border_width = 2,
    pad = 0,
  })
  back:clear_flag("SCROLLABLE")
  lvgl.container(back, {
    x = 6, y = 6, w = CARD_W - 12, h = CARD_H - 12,
    bg_opa = 0,
    border_color = C.gold_dim,
    border_width = 1,
    radius = 7,
    pad = 0,
  })
  lvgl.label(back, styled({ text = "♠", align = "center", y = -16,
                            text_color = C.gold }, f_title))
  lvgl.label(back, styled({ text = "21", align = "center", y = 40,
                            text_color = C.gold }, f_ui))

  local front = lvgl.container(root, {
    w = CARD_W, h = CARD_H,
    bg_color = C.cream,
    radius = 10,
    border_color = C.card_edge,
    border_width = 2,
    pad = 0,
  })
  front:clear_flag("SCROLLABLE")

  local color = RED[suit_i] and C.red or C.black
  lvgl.label(front, styled({ text = RANKS[rank_i], x = 8, y = 2,
                             text_color = color }, f_rank))
  lvgl.label(front, styled({ text = SUITS[suit_i], x = 8, y = 34,
                             text_color = color }, f_rank))
  lvgl.label(front, styled({ text = SUITS[suit_i], align = "center", y = -2,
                             text_color = color }, f_title))
  lvgl.label(front, styled({ text = RANKS[rank_i],
                             align = "bottom_right", x = -8, y = -4,
                             text_color = color }, f_ui))

  if face_up then
    back:add_flag("HIDDEN")
  else
    front:add_flag("HIDDEN")
  end
  root:move_foreground()

  return {
    root = root,
    front = front,
    back = back,
    rank = rank_i,
    suit = suit_i,
    value = VALUES[rank_i],
    up = face_up,
  }
end

local function layout_hand(hand, y)
  local n = #hand
  local step
  if n <= 2 then
    step = 106
  elseif n == 3 then
    step = 92
  elseif n == 4 then
    step = 78
  else
    step = 68
  end
  local total_w = CARD_W + (n - 1) * step
  local start = math.floor((W - total_w) / 2)
  for i, card in ipairs(hand) do
    card.root:set_pos(start + (i - 1) * step, y)
  end
end

local function update_deck_label()
  deck_lbl:set_text(tostring(#deck))
end

local function animate(card, delay, done, sound)
  local tx, ty = card.root:get_pos()
  card.root:set_pos(DECK_X, DECK_Y)
  card.root:add_flag("HIDDEN")
  anims[#anims + 1] = {
    root = card.root,
    sx = DECK_X,
    sy = DECK_Y,
    tx = tx,
    ty = ty,
    start = system.millis() + delay,
    duration = 300,
    shown = false,
    sound = sound,
    done = done,
  }
  card.root:move_foreground()
end

local function draw_one()
  if #deck == 0 then
    new_deck()
  end
  return table.remove(deck)
end

local function add_card(hand, y, face_up, delay, done)
  local data = draw_one()
  local card = make_card(data.rank, data.suit, face_up)
  hand[#hand + 1] = card
  layout_hand(hand, y)
  animate(card, delay or 0, done, "deal")
  update_deck_label()
  return card
end

local function set_message(text, color)
  message:set_text(text)
  message:set_style({ text_color = color })
end

local function update_scores()
  player_score:set_text(tostring(hand_total(player)))
  if dealer_revealed then
    dealer_score:set_text(tostring(hand_total(dealer)))
  elseif #dealer > 0 then
    dealer_score:set_text(tostring(hand_total({ dealer[1] })))
  else
    dealer_score:set_text("?")
  end
  score_lbl:set_text(string.format("胜 %d   负 %d   平 %d",
                                   wins, losses, pushes))
end

local function update_buttons()
  local function show(btn, on)
    if on then
      btn:clear_flag("HIDDEN")
    else
      btn:add_flag("HIDDEN")
    end
  end
  if state == "idle" or state == "result" then
    show(btn_deal, true)
    btn_deal:set_text(state == "result" and "下一局" or "发牌")
    show(btn_hit, false)
    show(btn_stand, false)
  elseif state == "player" then
    show(btn_deal, false)
    show(btn_hit, true)
    show(btn_stand, true)
  else
    show(btn_deal, false)
    show(btn_hit, false)
    show(btn_stand, false)
  end
end

local function clear_table()
  for i = #anims, 1, -1 do
    table.remove(anims, i)
  end
  for _, card in ipairs(player) do
    card.root:delete()
  end
  for _, card in ipairs(dealer) do
    card.root:delete()
  end
  player = {}
  dealer = {}
end

local function reveal_dealer()
  if dealer_revealed then
    return
  end
  dealer_revealed = true
  sfx("flip")
  for _, card in ipairs(dealer) do
    card.back:add_flag("HIDDEN")
    card.front:clear_flag("HIDDEN")
    card.up = true
  end
  update_scores()
end

local function finish(text, color, win_add, lose_add, push_add)
  state = "result"
  reveal_dealer()
  wins = wins + (win_add or 0)
  losses = losses + (lose_add or 0)
  pushes = pushes + (push_add or 0)
  update_scores()
  set_message(text, color)
  if text == "黑杰克！" then
    sfx("blackjack")
  elseif text == "爆牌！" then
    sfx("bust")
  elseif text == "庄家爆牌！" or win_add > 0 then
    sfx("win")
  elseif lose_add > 0 then
    sfx("lose")
  else
    sfx("push")
  end
  update_buttons()
end

local function stand()
  if state ~= "player" then
    return
  end
  state = "dealer"
  update_buttons()
  set_message("庄家回合", C.muted)
  reveal_dealer()
  dealer_next_at = system.millis() + 650
end

local function hit()
  if state ~= "player" then
    return
  end
  state = "hit_wait"
  update_buttons()
  set_message("发牌中…", C.muted)
  add_card(player, PLAYER_Y, true, 0, function()
    update_scores()
    local score = hand_total(player)
    if score > 21 then
      finish("爆牌！", C.lose, 0, 1, 0)
    else
      state = "player"
      if score == 21 then
        stand()
      else
        update_buttons()
        set_message("轮到你了", C.cream)
      end
    end
  end)
end

local function dealer_step()
  if state ~= "dealer" then
    return
  end
  local score = hand_total(dealer)
  if score < 17 then
    dealer_next_at = system.millis() + 10000
    set_message("庄家要牌", C.muted)
    add_card(dealer, DEALER_Y, true, 0, function()
      update_scores()
      dealer_next_at = system.millis() + 450
    end)
  else
    local ps = hand_total(player)
    if score > 21 then
      finish("庄家爆牌！", C.win, 1, 0, 0)
    elseif score > ps then
      finish("庄家胜", C.lose, 0, 1, 0)
    elseif score < ps then
      finish("你赢了！", C.win, 1, 0, 0)
    else
      finish("平局", C.push, 0, 0, 1)
    end
  end
end

local function after_deal()
  local ps = hand_total(player)
  local ds = hand_total(dealer)
  if ps == 21 or ds == 21 then
    reveal_dealer()
    if ps == 21 and ds == 21 then
      finish("平局", C.push, 0, 0, 1)
    elseif ps == 21 then
      finish("黑杰克！", C.win, 1, 0, 0)
    else
      finish("庄家黑杰克", C.lose, 0, 1, 0)
    end
  else
    state = "player"
    update_buttons()
    update_scores()
    set_message("轮到你了", C.cream)
  end
end

local function start_round()
  if state == "dealing" or state == "dealer" or state == "player" or
     state == "hit_wait" then
    return
  end
  clear_table()
  state = "dealing"
  dealer_revealed = false
  update_buttons()
  set_message("发牌中…", C.gold)
  dealer_score:set_text("?")
  player_score:set_text("0")

  if #deck < 15 then
    new_deck()
  end

  add_card(player, PLAYER_Y, true, 0)
  add_card(dealer, DEALER_Y, true, 170)
  add_card(player, PLAYER_Y, true, 340)
  add_card(dealer, DEALER_Y, false, 510)
  deal_ready_at = system.millis() + 930
  update_scores()
end

btn_deal:on("clicked", function()
  sfx("click")
  if state == "idle" or state == "result" then
    start_round()
  end
end)
btn_hit:on("clicked", function()
  sfx("click")
  hit()
end)
btn_stand:on("clicked", function()
  sfx("click")
  stand()
end)

math.randomseed((system.millis() % 2147483647) + 1)
new_deck()
update_deck_label()
update_scores()
update_buttons()
set_message("开始发牌", C.gold)
scr:load()

while not claw.abort_requested() do
  local now = system.millis()

  for i = #anims, 1, -1 do
    local a = anims[i]
    if now >= a.start then
      if not a.shown then
        a.root:clear_flag("HIDDEN")
        a.shown = true
        if a.sound then
          sfx(a.sound)
        end
      end
      local t = (now - a.start) / a.duration
      if t > 1 then
        t = 1
      end
      local ease = 1 - (1 - t) * (1 - t) * (1 - t)
      a.root:set_pos(math.floor(a.sx + (a.tx - a.sx) * ease),
                     math.floor(a.sy + (a.ty - a.sy) * ease))
      if t >= 1 then
        a.root:set_pos(a.tx, a.ty)
        table.remove(anims, i)
        if a.done then
          a.done()
        end
      end
    end
  end

  if state == "dealing" and #anims == 0 and now >= deal_ready_at then
    after_deal()
  elseif state == "dealer" and #anims == 0 and now >= dealer_next_at then
    dealer_step()
  end

  lvgl.process_events(20)
end

if f_ui then
  lvgl.set_default_font(nil)
end
for _, font in ipairs({ f_ui, f_rank, f_title }) do
  if font and font:is_valid() then
    font:delete()
  end
end
lvgl.deinit()
