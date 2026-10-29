-- Morse Station / 电码站 for the 1024x600 ESP32-P4 panel.
--
-- A lamp-and-key Morse game.  Two modes share one telegraph key:
--   * 发报训练 (TX): the target letter is shown; key its Morse code back.
--   * 抄收挑战 (RX): the letter is hidden; watch / hear the signal lamp and
--     key the same rhythm back.
-- The left panel is a live Morse tree: every dot / dash walks one branch and
-- lights the path, so a wrong turn is visible before the answer is revealed.
--
-- Assets: assets/ui_cn.ttf, assets/display.ttf, assets/img/bg.jpg,
--         assets/sfx/{dot,dash,key,ok,err,start,over}.ogg
-- Design notes: DESIGN.md.  Regenerate art with tools/make_morse_*.py.

local lvgl = require("lvgl")
local system = require("system")
local ok_audio, audio = pcall(require, "audio")
local ok_image, image = pcall(require, "image")

local W, H = 1024, 600

local app_src = debug.getinfo(1, "S").source or ""
if app_src:sub(1, 1) == "@" then
  app_src = app_src:sub(2)
end
local app_dir = app_src:match("^(.*)/scripts/[^/]+$") or
                "/fatfs/claw/data/apps/morse_station"

local BEST_PATH = "/fatfs/claw/data/morse_station_best.txt"
local MAX_LIVES = 3
local RX_UNIT = 170          -- ms: one dot, one intra-letter gap
local DASH_MS = 280          -- hold longer than this = dash
local HOLD_METER_MS = 620    -- full scale of the key hold meter
local METER_W = 380
local WIRE_LEN = 252         -- console signal-window bar width

-- ---------------------------------------------------------------- palette

local C = {
  bg         = "#040806",
  panel      = "#07110d",
  panel2     = "#0a1812",
  card       = "#0a1812",
  line       = "#173326",
  line_dim   = "#10251b",
  node       = "#09160f",
  node_border= "#24503c",
  letter     = "#a9c9b9",
  mint       = "#6ff0b8",
  mint_dim   = "#3f9d76",
  mint_deep  = "#1d5c42",
  brass      = "#d9b56d",
  brass_hi   = "#efcf8a",
  brass_dim  = "#7a5f30",
  cream      = "#f2ecda",
  text       = "#d9eadf",
  muted      = "#7c9488",
  dim        = "#4f6a5e",
  danger     = "#ff6b5c",
  danger_dim = "#7a2d24",
  amber      = "#ffb454",
  tape       = "#050b08",
}

-- ---------------------------------------------------------------- morse data

local MORSE = {
  A = ".-",   B = "-...", C = "-.-.", D = "-..",  E = ".",    F = "..-.",
  G = "--.",  H = "....", I = "..",   J = ".---", K = "-.-",  L = ".-..",
  M = "--",   N = "-.",   O = "---",  P = ".--.", Q = "--.-", R = ".-.",
  S = "...",  T = "-",    U = "..-",  V = "...-", W = ".--",  X = "-..-",
  Y = "-.--", Z = "--..",
}

local ALL_LETTERS = {
  "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
  "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
}

local POOL_1 = { "E", "T", "A", "I", "M", "N" }
local POOL_2 = {
  "E", "T", "A", "I", "M", "N",
  "S", "U", "R", "W", "D", "K", "G", "O",
}

-- ---------------------------------------------------------------- helpers

local F = {}

F.mk_label = function(parent, opts, font)
  if font then
    opts.font = font
  end
  return lvgl.label(parent, opts)
end

F.mk_box = function(parent, opts)
  local o = lvgl.container(parent, opts)
  o:clear_flag("SCROLLABLE")
  return o
end

F.mk_btn = function(parent, opts, font)
  if font then
    opts.font = font
  end
  local b = lvgl.button(parent, opts)
  b:clear_flag("SCROLLABLE")
  return b
end

F.passthrough = function(o)
  o:clear_flag("CLICKABLE")
  o:clear_flag("SCROLLABLE")
  return o
end

F.clamp01 = function(x)
  if x < 0 then
    return 0
  elseif x > 1 then
    return 1
  end
  return x
end

F.fmt5 = function(n)
  return string.format("%05d", math.floor(n))
end

F.pretty_code = function(code)
  local out = {}
  for i = 1, #code do
    out[#out + 1] = code:sub(i, i) == "." and "·" or "−"
  end
  return table.concat(out, " ")
end

F.sfx = function(name)
  if ok_audio and audio and audio.play then
    pcall(audio.play, name)
  end
end

local best = 0
do
  local ok, f = pcall(io.open, BEST_PATH, "r")
  if ok and f then
    local raw = f:read("*a")
    f:close()
    local n = tonumber(raw)
    if n and n > 0 then
      best = math.floor(n)
    end
  end
end

-- ---------------------------------------------------------------- runtime

lvgl.init({ buffer_lines = 40, tick_ms = 5, task_period_ms = 10 })

F.load_font = function(file, px, cache)
  local ok, font = pcall(lvgl.font_load, app_dir .. "/assets/" .. file,
                         { size = px, cache_size = cache })
  if ok and font then
    return font
  end
  return nil
end

local f_small = F.load_font("ui_cn.ttf", 16, 128)
local f_ui = F.load_font("ui_cn.ttf", 20, 128)
local f_num = F.load_font("ui_cn.ttf", 40, 96)
local f_disp = F.load_font("display.ttf", 36, 64)
local f_big = F.load_font("display.ttf", 96, 40)
if f_ui then
  lvgl.set_default_font(f_ui)
end

math.randomseed((system.millis() % 2147483647) + 1)

-- ---------------------------------------------------------------- state

local state = "menu"         -- menu | intro | input | receive | judge | gameover
local mode = "tx"
local score = 0
local lives = MAX_LIVES
local round_no = 1
local streak = 0
local best_combo = 0
local correct_count = 0
local attempts = 0
local new_record = false

local target_letter = ""
local target_code = ""
local input_code = ""
local last_target = nil

local deadline = 0
local limit_ms = 12000
local remaining_ms = 0
local replays = 0
local hint_used = false

local cue = nil
local intro_start = 0
local intro_until = 0
local judge_until = 0
local over_at = 0
local over_shown = false

local key_pressed = false
local key_press_at = 0
local key_latched = false
local key_latch_at = 0
local meter_visible = false
local meter_hide_at = 0
local meter_w = 1
local meter_color = C.mint

local timer_w = WIRE_LEN
local timer_color = C.mint
local timer_text = ""

local pip_hot = { false, false, false, false }
local pip_active = { nil, nil, nil, nil }
local pip_hide_at = { 0, 0, 0, 0 }

local float_state = { active = false, t0 = 0 }
local spark_state = {
  active = false, t0 = 0,
  from_x = 0, from_y = 0, to_x = 0, to_y = 0,
}
local card_glow_state = { active = false, t0 = 0 }
local node_glow_state = {
  active = false, t0 = 0, cx = 0, cy = 0, base = 30,
}

-- ---------------------------------------------------------------- screen

local scr = lvgl.create_screen()
scr:set_style({ bg_color = C.bg, pad = 0 })
scr:clear_flag("SCROLLABLE")

local bg = lvgl.image(scr, { x = 0, y = 0, w = W, h = H, pad = 0 })
bg:clear_flag("CLICKABLE")
if ok_image and image then
  local ok, frame = pcall(image.load_file, app_dir .. "/assets/img/bg.jpg")
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

-- ---------------------------------------------------------------- header

local hdr_word = F.mk_label(scr, {
  text = "MORSE STATION", x = 36, y = 10, text_color = C.cream,
}, f_disp)
local hdr_sub = F.mk_label(scr, {
  text = "电 码 站　·　7.023 MHz", x = 38, y = 54, text_color = C.mint_dim,
}, f_small)

local mode_dot = F.mk_box(scr, {
  x = 390, y = 22, w = 8, h = 8, radius = 4,
  bg_color = C.mint_dim, border_width = 0, pad = 0,
})
F.passthrough(mode_dot)
local mode_lbl = F.mk_label(scr, {
  text = "待机", x = 404, y = 10, text_color = C.cream,
}, f_ui)
local round_lbl = F.mk_label(scr, {
  text = "选择发报或抄收", x = 404, y = 42, text_color = C.muted,
}, f_small)

local score_cap = F.mk_label(scr, {
  text = "得 分", x = 680, y = 6, text_color = C.muted,
}, f_small)
local score_lbl = F.mk_label(scr, {
  text = "00000", x = 680, y = 22, text_color = C.cream,
}, f_num)
local combo_cap = F.mk_label(scr, {
  text = "连 击", x = 806, y = 6, text_color = C.muted,
}, f_small)
local combo_lbl = F.mk_label(scr, {
  text = "×1.0", x = 806, y = 30, text_color = C.muted,
}, f_ui)
local lives_cap = F.mk_label(scr, {
  text = "信 号", x = 910, y = 6, text_color = C.muted,
}, f_small)

local life_dots = {}
for i = 1, MAX_LIVES do
  life_dots[i] = F.mk_box(scr, {
    x = 910 + (i - 1) * 24, y = 38, w = 18, h = 18, radius = 9,
    bg_color = C.mint, border_color = C.mint_dim, border_width = 1, pad = 0,
  })
  F.passthrough(life_dots[i])
end

lvgl.line(scr, {
  points = { { x = 36, y = 84 }, { x = 988, y = 84 } },
  line_color = C.line_dim, line_width = 1,
})
lvgl.line(scr, {
  points = { { x = 36, y = 84 }, { x = 148, y = 84 } },
  line_color = C.brass_dim, line_width = 2,
})

-- ---------------------------------------------------------------- tree panel

local TREE_CX, TREE_Y0, TREE_DY = 362, 172, 64
local NODE_R = { [0] = 9, [1] = 18, [2] = 16, [3] = 14, [4] = 12 }

local tree_panel = F.mk_box(scr, {
  x = 28, y = 104, w = 668, h = 396,
  bg_color = C.panel, bg_opa = 238, radius = 18,
  border_color = C.line, border_width = 1, pad = 0,
})
local tree_inner = F.mk_box(tree_panel, {
  x = 12, y = 12, w = 644, h = 372,
  bg_opa = 0, radius = 14,
  border_color = C.line_dim, border_width = 1, pad = 0,
})
F.passthrough(tree_inner)
F.mk_label(tree_panel, {
  text = "解 码 树", x = 20, y = 12, text_color = C.mint_dim,
}, f_small)
F.mk_label(tree_panel, {
  text = "26 字 母 · 4 层", align = "top_right", x = -20, y = 12,
  text_color = C.dim,
}, f_small)
F.mk_label(tree_panel, {
  text = "左 · 点　　右 − 划　　路径点亮 = 当前电码",
  x = 20, y = 356, text_color = C.dim,
}, f_small)

local codes_by_depth = {}
local tree_pos = {}
local tree_edges = {}
local tree_nodes = {}
local letters_by_code = {}
for _, letter in ipairs(ALL_LETTERS) do
  letters_by_code[MORSE[letter]] = letter
end

do
  local level = { "" }
  codes_by_depth[0] = { "" }
  for d = 1, 4 do
    local nxt = {}
    for _, code in ipairs(level) do
      nxt[#nxt + 1] = code .. "."
      nxt[#nxt + 1] = code .. "-"
    end
    codes_by_depth[d] = nxt
    level = nxt
  end
  for d = 0, 4 do
    local list = codes_by_depth[d]
    local n = #list
    local spacing = (d == 0) and 0 or (608 / n)
    for i, code in ipairs(list) do
      local x, y
      if d == 0 then
        x, y = TREE_CX, TREE_Y0
      else
        x = TREE_CX + (i - (n + 1) / 2) * spacing
        y = TREE_Y0 + d * TREE_DY
      end
      tree_pos[code] = { x = math.floor(x + 0.5), y = y, d = d }
    end
  end
end

-- edges first so the nodes sit on top
for d = 1, 4 do
  for _, code in ipairs(codes_by_depth[d]) do
    local parent = code:sub(1, d - 1)
    local a, b = tree_pos[parent], tree_pos[code]
    tree_edges[code] = lvgl.line(scr, {
      points = { { x = a.x, y = a.y }, { x = b.x, y = b.y } },
      line_color = C.line, line_width = 2,
    })
  end
end

for d = 0, 4 do
  for _, code in ipairs(codes_by_depth[d]) do
    local p = tree_pos[code]
    local letter = letters_by_code[code]
    local is_empty = (d == 4 and not letter)
    local node, lbl
    if is_empty then
      local sz = 10
      node = F.mk_box(scr, {
        x = p.x - sz // 2, y = p.y - sz // 2, w = sz, h = sz,
        radius = sz // 2, bg_opa = 0,
        border_color = C.line, border_width = 1, pad = 0,
      })
    else
      local r = NODE_R[d]
      node = F.mk_box(scr, {
        x = p.x - r, y = p.y - r, w = 2 * r, h = 2 * r,
        radius = r, bg_color = C.node,
        border_color = C.node_border, border_width = 1, pad = 0,
      })
      if d == 0 then
        local dot = F.mk_box(node, {
          align = "center", w = 6, h = 6, radius = 3,
          bg_color = C.brass, border_width = 0, pad = 0,
        })
        F.passthrough(dot)
      else
        lbl = F.mk_label(node, {
          text = letter, align = "center", y = 0, text_color = C.letter,
        }, (d <= 2) and f_ui or f_small)
        F.passthrough(lbl)
      end
    end
    tree_nodes[code] = {
      obj = node, label = lbl, x = p.x, y = p.y,
      r = is_empty and 5 or NODE_R[d], depth = d, empty = is_empty,
    }
  end
end

local spark = F.mk_box(scr, {
  x = -20, y = -20, w = 14, h = 14, radius = 7,
  bg_color = C.mint, border_color = C.cream, border_width = 1, pad = 0,
})
spark:add_flag("HIDDEN")

local node_glow = F.mk_box(scr, {
  x = -60, y = -60, w = 30, h = 30, radius = 999,
  bg_opa = 0, border_color = C.brass, border_width = 2, pad = 0,
})
node_glow:add_flag("HIDDEN")

-- ---------------------------------------------------------------- console

local console = F.mk_box(scr, {
  x = 708, y = 104, w = 288, h = 396,
  bg_color = C.panel, bg_opa = 238, radius = 18,
  border_color = C.line, border_width = 1, pad = 0,
})
local console_inner = F.mk_box(console, {
  x = 12, y = 12, w = 264, h = 372,
  bg_opa = 0, radius = 14,
  border_color = C.line_dim, border_width = 1, pad = 0,
})
F.passthrough(console_inner)
F.mk_label(console, {
  text = "报 文", x = 20, y = 12, text_color = C.mint_dim,
}, f_small)
F.mk_label(console, {
  text = "7.023 MHz", align = "top_right", x = -20, y = 12,
  text_color = C.dim,
}, f_small)

local CARD_X, CARD_Y, CARD_W, CARD_H = 18, 42, 252, 146
local card = F.mk_box(console, {
  x = CARD_X, y = CARD_Y, w = CARD_W, h = CARD_H,
  bg_color = C.card, radius = 14,
  border_color = C.line, border_width = 1, pad = 0,
})
local card_tag = F.mk_label(card, {
  text = "目 标", x = 14, y = 8, text_color = C.muted,
}, f_small)
local card_mode = F.mk_label(card, {
  text = "STBY", align = "top_right", x = -14, y = 8, text_color = C.dim,
}, f_small)
local card_letter = F.mk_label(card, {
  text = "A", align = "center", y = 6, text_color = C.cream,
}, f_big)
F.passthrough(card_letter)

local orb = F.mk_box(card, {
  x = 98, y = 52, w = 56, h = 56, radius = 28,
  bg_color = "#0c2a1d", border_color = C.mint_deep, border_width = 1, pad = 0,
})
local orb_core = F.mk_box(orb, {
  align = "center", w = 22, h = 22, radius = 11,
  bg_color = "#0c2a1d", border_width = 0, pad = 0,
})
local orb_pin = F.mk_box(orb, {
  align = "center", w = 8, h = 8, radius = 4,
  bg_color = C.mint_dim, border_width = 0, pad = 0,
})
F.passthrough(orb_core)
F.passthrough(orb_pin)
orb:add_flag("HIDDEN")

local ring1 = F.mk_box(card, {
  align = "center", y = 7, w = 56, h = 56, radius = 999,
  bg_opa = 0, border_color = C.mint, border_width = 2, pad = 0,
})
local ring2 = F.mk_box(card, {
  align = "center", y = 7, w = 56, h = 56, radius = 999,
  bg_opa = 0, border_color = C.mint, border_width = 1, pad = 0,
})
ring1:add_flag("HIDDEN")
ring2:add_flag("HIDDEN")
F.passthrough(ring1)
F.passthrough(ring2)

local card_glow = F.mk_box(console, {
  x = 14, y = 38, w = 260, h = 154, radius = 18,
  bg_opa = 0, border_color = C.mint, border_width = 2, pad = 0,
})
card_glow:add_flag("HIDDEN")
F.passthrough(card_glow)

-- corner brackets: a thin brass technical frame around the card
local bracket_len = 14
local bracket_inset = 4
F.bracket = function(x, y, horizontal, flip_x, flip_y)
  local bw, bh = horizontal and bracket_len or 2, horizontal and 2 or bracket_len
  local ox = flip_x and -bw or 0
  local oy = flip_y and -bh or 0
  local b = F.mk_box(console, {
    x = x + ox, y = y + oy, w = bw, h = bh,
    bg_color = C.brass_dim, border_width = 0, pad = 0,
  })
  F.passthrough(b)
end
F.bracket(CARD_X + bracket_inset, CARD_Y + bracket_inset, true, false, false)
F.bracket(CARD_X + bracket_inset, CARD_Y + bracket_inset, false, false, false)
F.bracket(CARD_X + CARD_W - bracket_inset, CARD_Y + bracket_inset, true, true, false)
F.bracket(CARD_X + CARD_W - bracket_inset, CARD_Y + bracket_inset, false, true, false)
F.bracket(CARD_X + bracket_inset, CARD_Y + CARD_H - bracket_inset, true, false, true)
F.bracket(CARD_X + bracket_inset, CARD_Y + CARD_H - bracket_inset, false, false, true)
F.bracket(CARD_X + CARD_W - bracket_inset, CARD_Y + CARD_H - bracket_inset, true, true, true)
F.bracket(CARD_X + CARD_W - bracket_inset, CARD_Y + CARD_H - bracket_inset, false, true, true)

F.mk_label(console, {
  text = "纸 带", x = 18, y = 192, text_color = C.muted,
}, f_small)

local tape = F.mk_box(console, {
  x = 18, y = 218, w = 252, h = 44,
  bg_color = C.tape, radius = 8,
  border_color = C.line, border_width = 1, pad = 0,
})
local slots = {}
for i = 1, 4 do
  local slot = F.mk_box(tape, {
    x = 22 + (i - 1) * 54, y = 9, w = 46, h = 26,
    bg_color = "#050b08", radius = 6,
    border_color = C.line_dim, border_width = 1, pad = 0,
  })
  local dot = F.mk_box(slot, {
    align = "center", w = 16, h = 16, radius = 8,
    bg_color = C.tape, border_color = C.mint_dim, border_width = 2, pad = 0,
  })
  local dash = F.mk_box(slot, {
    align = "center", w = 32, h = 10, radius = 5,
    bg_color = C.tape, border_color = C.mint_dim, border_width = 2, pad = 0,
  })
  dot:add_flag("HIDDEN")
  dash:add_flag("HIDDEN")
  F.passthrough(dot)
  F.passthrough(dash)
  slots[i] = { slot = slot, dot = dot, dash = dash }
end
local caret = F.mk_box(tape, {
  x = 22, y = 9, w = 2, h = 26, radius = 1,
  bg_color = C.amber, border_width = 0, pad = 0,
})
caret:add_flag("HIDDEN")
F.passthrough(caret)

local status_lbl = F.mk_label(console, {
  text = "待机", align = "top_mid", y = 270, text_color = C.muted,
}, f_ui)
local sub_lbl = F.mk_label(console, {
  text = "选择模式开始通联", align = "top_mid", y = 300,
  text_color = C.dim,
}, f_small)

local timer_track = F.mk_box(console, {
  x = 18, y = 328, w = 252, h = 8, bg_color = "#0c1a14",
  radius = 4, border_width = 0, pad = 0,
})
local timer_fill = F.mk_box(timer_track, {
  x = 0, y = 0, w = 252, h = 8, bg_color = C.mint,
  radius = 4, border_width = 0, pad = 0,
})
for _, tx in ipairs({ 63, 126, 189 }) do
  local tick = F.mk_box(timer_track, {
    x = tx, y = 0, w = 1, h = 8, bg_color = C.panel,
    bg_opa = 200, border_width = 0, pad = 0,
  })
  F.passthrough(tick)
end
F.passthrough(timer_fill)
local timer_cap = F.mk_label(console, {
  text = "信 号 窗 口", x = 18, y = 344, text_color = C.dim,
}, f_small)
local timer_lbl = F.mk_label(console, {
  text = "12.0s", align = "top_right", x = -18, y = 344,
  text_color = C.muted,
}, f_small)

-- ---------------------------------------------------------------- right controls

local best_lbl = F.mk_label(scr, {
  text = "最佳 " .. F.fmt5(best), x = 720, y = 504, text_color = C.muted,
}, f_small)
local aux_btn = F.mk_btn(scr, {
  text = "提示 -30%", x = 720, y = 526, w = 132, h = 44,
  bg_color = C.panel2, text_color = C.brass,
  border_color = C.line, border_width = 1, radius = 12, pad = 0,
}, f_small)
local menu_btn = F.mk_btn(scr, {
  text = "菜单", x = 864, y = 526, w = 132, h = 44,
  bg_color = C.panel2, text_color = C.cream,
  border_color = C.line, border_width = 1, radius = 12, pad = 0,
}, f_small)
local hits_lbl = F.mk_label(scr, {
  text = "命中 0/0", x = 720, y = 572, text_color = C.dim,
}, f_small)
aux_btn:add_flag("HIDDEN")

-- ---------------------------------------------------------------- telegraph key

local key = F.mk_btn(scr, {
  x = 28, y = 508, w = 680, h = 84,
  bg_color = "#0a1510", border_color = "#3a5a44", border_width = 1,
  radius = 16, pad = 0,
})
local key_title = F.mk_label(key, {
  text = "发 报 键", x = 28, y = 12, text_color = C.brass,
}, f_ui)
local key_sub = F.mk_label(key, {
  text = "轻点 ·　按住 −", x = 28, y = 42, text_color = C.muted,
}, f_small)
F.passthrough(key_title)
F.passthrough(key_sub)

local pivot = F.mk_box(key, {
  x = 190, y = 46, w = 20, h = 20, radius = 10,
  bg_color = "#0a1510", border_color = C.brass_dim, border_width = 1, pad = 0,
})
local lever = F.mk_box(key, {
  x = 200, y = 54, w = 400, h = 4, radius = 2,
  bg_color = C.brass_dim, border_width = 0, pad = 0,
})
local knob = F.mk_box(key, {
  x = 592, y = 32, w = 48, h = 48, radius = 24,
  bg_color = "#1b1609", border_color = C.brass, border_width = 2, pad = 0,
})
local knob_in = F.mk_box(knob, {
  x = 7, y = 7, w = 34, h = 34, radius = 17,
  bg_color = "#2a2210", border_color = C.brass_dim, border_width = 1, pad = 0,
})
local knob_lbl = F.mk_label(knob, {
  text = "·−", align = "center", text_color = C.brass_dim,
}, f_small)
F.passthrough(pivot)
F.passthrough(lever)
F.passthrough(knob)
F.passthrough(knob_in)
F.passthrough(knob_lbl)

local meter = F.mk_box(key, {
  x = 200, y = 72, w = METER_W, h = 3, bg_color = "#0d1d15",
  radius = 1, border_width = 0, pad = 0,
})
local meter_fill = F.mk_box(meter, {
  x = 0, y = 0, w = 1, h = 3, bg_color = C.mint,
  radius = 1, border_width = 0, pad = 0,
})
local meter_tick = F.mk_box(meter, {
  x = 190, y = 0, w = 1, h = 3, bg_color = C.brass_dim,
  border_width = 0, pad = 0,
})
meter_fill:add_flag("HIDDEN")
F.passthrough(meter)
F.passthrough(meter_fill)
F.passthrough(meter_tick)

local float_lbl = F.mk_label(scr, {
  text = "+100", x = 900, y = 250, text_color = C.mint,
}, f_num)
F.passthrough(float_lbl)
float_lbl:add_flag("HIDDEN")

-- ---------------------------------------------------------------- menu overlay

local menu_dim = F.mk_box(scr, {
  x = 0, y = 0, w = W, h = H,
  bg_color = "#020604", bg_opa = 222, border_width = 0, pad = 0,
})
menu_dim:add_flag("CLICKABLE")
local menu_card = F.mk_box(menu_dim, {
  x = 172, y = 64, w = 680, h = 472,
  bg_color = C.panel, radius = 26,
  border_color = C.line, border_width = 1, pad = 0,
})
local menu_inner = F.mk_box(menu_card, {
  x = 12, y = 12, w = 656, h = 448, bg_opa = 0,
  radius = 20, border_color = C.line_dim, border_width = 1, pad = 0,
})
F.passthrough(menu_inner)

F.mk_label(menu_card, {
  text = "SIGNAL STATION · 7.023 MHz", align = "top_mid", y = 24,
  text_color = C.brass_dim,
}, f_small)
F.mk_label(menu_card, {
  text = "电 码 站", align = "top_mid", y = 44, text_color = C.cream,
}, f_big)
F.mk_label(menu_card, {
  text = "把一束电波，敲成一句话。", align = "top_mid", y = 186,
  text_color = C.muted,
}, f_ui)

local menu_ring1 = F.mk_box(menu_card, {
  align = "top_right", x = -26, y = 14, w = 40, h = 40, radius = 999,
  bg_opa = 0, border_color = C.mint_deep, border_width = 1, pad = 0,
})
local menu_ring2 = F.mk_box(menu_card, {
  align = "top_right", x = -34, y = 6, w = 56, h = 56, radius = 999,
  bg_opa = 0, border_color = C.line, border_width = 1, pad = 0,
})
F.passthrough(menu_ring1)
F.passthrough(menu_ring2)

F.bind_press_style = function(btn, normal, pressed)
  btn:on("pressed", function()
    btn:set_style({ bg_color = pressed })
  end)
  btn:on("released", function()
    btn:set_style({ bg_color = normal })
  end)
end

F.bind_press_style(aux_btn, C.panel2, "#12251b")
F.bind_press_style(menu_btn, C.panel2, "#12251b")

F.make_mode_card = function(x, title, sub, hint, big, code, color)
  local b = F.mk_btn(menu_card, {
    x = x, y = 228, w = 292, h = 148,
    bg_color = "#0b1913", border_color = C.line, border_width = 1,
    radius = 16, pad = 0,
  })
  local t = F.mk_label(b, { text = title, x = 22, y = 12, text_color = C.cream }, f_num)
  local s = F.mk_label(b, { text = sub, x = 22, y = 76, text_color = C.muted }, f_small)
  local h = F.mk_label(b, { text = hint, x = 22, y = 104, text_color = C.dim }, f_small)
  local bl = F.mk_label(b, { text = big, x = 218, y = 12, text_color = color }, f_num)
  local cl = F.mk_label(b, { text = code, x = 220, y = 76, text_color = color }, f_ui)
  F.passthrough(t)
  F.passthrough(s)
  F.passthrough(h)
  F.passthrough(bl)
  F.passthrough(cl)
  F.bind_press_style(b, "#0b1913", "#12251b")
  return b
end

local mode_tx = F.make_mode_card(40, "发 报", "看字 · 敲出电码", "训练手键记忆",
                               "A", "· −", C.mint)
local mode_rx = F.make_mode_card(348, "抄 收", "看灯 · 还原电文", "训练听辨节奏",
                               "?", "− · −", C.amber)

F.mk_label(menu_card, {
  text = "电键：轻点 = ·　　按住 = −", align = "top_mid", y = 384,
  text_color = C.cream,
}, f_ui)
local menu_best = F.mk_label(menu_card, {
  text = "本台最高 " .. F.fmt5(best), align = "top_mid", y = 414,
  text_color = C.muted,
}, f_small)
F.mk_label(menu_card, {
  text = "26 个字母 · 3 盏信号灯 · 连击加成", align = "top_mid", y = 438,
  text_color = C.dim,
}, f_small)

-- ---------------------------------------------------------------- game-over overlay

local over_dim = F.mk_box(scr, {
  x = 0, y = 0, w = W, h = H,
  bg_color = "#020604", bg_opa = 222, border_width = 0, pad = 0,
})
over_dim:add_flag("CLICKABLE")
over_dim:add_flag("HIDDEN")
local over_card = F.mk_box(over_dim, {
  x = 242, y = 104, w = 540, h = 392,
  bg_color = C.panel, radius = 24,
  border_color = C.line, border_width = 1, pad = 0,
})
local over_inner = F.mk_box(over_card, {
  x = 12, y = 12, w = 516, h = 368, bg_opa = 0,
  radius = 18, border_color = C.line_dim, border_width = 1, pad = 0,
})
F.passthrough(over_inner)

F.mk_label(over_card, {
  text = "TRANSMISSION ENDED", align = "top_mid", y = 24,
  text_color = C.brass_dim,
}, f_small)
F.mk_label(over_card, {
  text = "电 文 中 断", align = "top_mid", y = 48,
  text_color = C.cream,
}, f_num)
F.mk_label(over_card, {
  text = "本 局 得 分", align = "top_mid", y = 106,
  text_color = C.muted,
}, f_small)
local over_score = F.mk_label(over_card, {
  text = "00000", align = "top_mid", y = 128,
  text_color = C.mint,
}, f_big)
local over_stats = F.mk_label(over_card, {
  text = "命中 0/0　最高连击 0　准确率 0%", align = "top_mid", y = 272,
  text_color = C.muted,
}, f_ui)
local over_record = F.mk_label(over_card, {
  text = "新 纪 录", align = "top_mid", y = 300,
  text_color = C.brass,
}, f_ui)
over_record:add_flag("HIDDEN")

local over_again = F.mk_btn(over_card, {
  text = "再来一局", x = 60, y = 326, w = 200, h = 50,
  bg_color = C.brass, text_color = "#0a1510",
  border_color = C.brass_dim, border_width = 1, radius = 14, pad = 0,
}, f_ui)
local over_menu = F.mk_btn(over_card, {
  text = "返回菜单", x = 280, y = 326, w = 200, h = 50,
  bg_color = C.panel2, text_color = C.cream,
  border_color = C.line, border_width = 1, radius = 14, pad = 0,
}, f_ui)
F.bind_press_style(over_again, C.brass, C.brass_hi)
F.bind_press_style(over_menu, C.panel2, "#12251b")

-- ---------------------------------------------------------------- UI helpers

F.set_status = function(text, color)
  status_lbl:set_text(text)
  status_lbl:set_style({ text_color = color })
end

F.set_sub = function(text, color)
  sub_lbl:set_text(text)
  sub_lbl:set_style({ text_color = color })
end

F.update_score = function()
  score_lbl:set_text(F.fmt5(score))
end

F.update_combo = function()
  local mult = 1 + math.min(streak, 10) * 0.1
  combo_lbl:set_text(string.format("×%.1f", mult))
  combo_lbl:set_style({ text_color = streak > 0 and C.mint or C.muted })
end

F.update_lives = function()
  for i = 1, MAX_LIVES do
    local on = i <= lives
    life_dots[i]:set_style({
      bg_color = on and C.mint or "#1a0e0c",
      border_color = on and C.mint_dim or C.danger_dim,
      opa = on and 255 or 150,
    })
  end
end

F.update_hits = function()
  hits_lbl:set_text(string.format("命中 %d/%d", correct_count, attempts))
  best_lbl:set_text("最佳 " .. F.fmt5(best))
end

F.show_orb = function()
  card_letter:add_flag("HIDDEN")
  orb_pin:set_style({ bg_color = C.mint_dim })
  orb_core:set_style({ bg_color = "#0c2a1d" })
  orb:set_style({ bg_color = "#0c2a1d", border_color = C.mint_deep })
  orb:clear_flag("HIDDEN")
end

F.hide_orb = function()
  orb:add_flag("HIDDEN")
  ring1:add_flag("HIDDEN")
  ring2:add_flag("HIDDEN")
end

F.reveal_card = function()
  orb:add_flag("HIDDEN")
  ring1:add_flag("HIDDEN")
  ring2:add_flag("HIDDEN")
  card_letter:set_text(target_letter)
  card_letter:set_style({ text_color = C.cream })
  card_letter:clear_flag("HIDDEN")
end

-- ---------------------------------------------------------------- tree paint

F.reset_tree = function()
  for _, e in pairs(tree_edges) do
    e:set_style({ line_color = C.line, line_width = 2 })
  end
  for _, n in pairs(tree_nodes) do
    if n.empty then
      n.obj:set_style({ bg_opa = 0, border_color = C.line, border_width = 1 })
    else
      n.obj:set_style({
        bg_color = C.node, bg_opa = 255,
        border_color = C.node_border, border_width = 1,
      })
      if n.label then
        n.label:set_style({ text_color = C.letter })
      end
    end
  end
end

F.paint_path = function(code, line_color, width, node_bg, node_border, label_color)
  local root = tree_nodes[""]
  if root then
    root.obj:set_style({
      bg_color = node_bg, border_color = node_border, border_width = 2,
    })
  end
  for i = 1, #code do
    local c = code:sub(1, i)
    local e = tree_edges[c]
    if e then
      e:set_style({ line_color = line_color, line_width = width })
    end
    local n = tree_nodes[c]
    if n then
      if n.empty then
        n.obj:set_style({ border_color = node_border, border_width = 2 })
      else
        n.obj:set_style({
          bg_color = node_bg, border_color = node_border, border_width = 2,
        })
        if n.label then
          n.label:set_style({ text_color = label_color })
        end
      end
    end
  end
end

F.show_live_path = function()
  F.reset_tree()
  if input_code ~= "" then
    F.paint_path(input_code, C.mint, 4, "#0e2a1e", C.mint, C.cream)
  end
end

F.show_result = function(tcode, pcode, ok)
  F.reset_tree()
  if not ok and pcode ~= "" then
    F.paint_path(pcode, C.danger, 3, "#2a0e0c", C.danger, C.danger)
  end
  F.paint_path(tcode, ok and C.mint or C.brass, 4,
             "#1b2414", ok and C.mint or C.brass, C.cream)
  local tn = tree_nodes[tcode]
  if tn and not tn.empty then
    tn.obj:set_style({
      bg_color = "#241f0e", border_color = C.brass, border_width = 2,
    })
    if tn.label then
      tn.label:set_style({ text_color = C.brass })
    end
  end
end

-- ---------------------------------------------------------------- tape

F.set_pip = function(i, sym, now)
  local s = slots[i]
  if not s then
    return
  end
  s.dot:add_flag("HIDDEN")
  s.dash:add_flag("HIDDEN")
  local pip = (sym == ".") and s.dot or s.dash
  pip:clear_flag("HIDDEN")
  pip:set_style({ border_color = C.mint })
  pip_active[i] = pip
  pip_hot[i] = true
  pip_hide_at[i] = now + 280
end

F.clear_tape = function()
  for i = 1, 4 do
    slots[i].dot:add_flag("HIDDEN")
    slots[i].dash:add_flag("HIDDEN")
    pip_hot[i] = false
    pip_active[i] = nil
  end
  caret:add_flag("HIDDEN")
end

F.show_tape_code = function(code, color)
  color = color or C.mint_dim
  F.clear_tape()
  for i = 1, #code do
    local s = slots[i]
    if s then
      local pip = (code:sub(i, i) == ".") and s.dot or s.dash
      pip:clear_flag("HIDDEN")
      pip:set_style({ border_color = color })
    end
  end
end

-- ---------------------------------------------------------------- cue / lamp

F.reset_lamp_visual = function()
  cue = nil
  card:set_style({ border_color = C.line, border_width = 1 })
  orb_pin:set_style({ bg_color = C.mint_dim })
  orb_core:set_style({ bg_color = "#0c2a1d" })
  orb:set_style({ bg_color = "#0c2a1d", border_color = C.mint_deep })
  ring1:add_flag("HIDDEN")
  ring2:add_flag("HIDDEN")
  caret:add_flag("HIDDEN")
  mode_dot:set_style({ opa = 255 })
end

F.apply_cue_event = function(ev, now)
  if not cue then
    return
  end
  if ev.on then
    cue.lamp_on = true
    cue.lamp_start = now
    cue.lamp_dur = ev.dur
    cue.lamp_sym = ev.sym
    if cue.kind == "rx" then
      orb_pin:set_style({ bg_color = C.cream })
      orb_core:set_style({ bg_color = C.mint })
      orb:set_style({ bg_color = "#0e2a1e", border_color = C.mint })
      ring1:clear_flag("HIDDEN")
      ring2:clear_flag("HIDDEN")
      ring1:set_size(56, 56)
      ring2:set_size(56, 56)
      ring1:set_style({ opa = 230 })
      ring2:set_style({ opa = 180 })
      mode_dot:set_style({ opa = 255 })
    end
    card:set_style({ border_color = C.mint, border_width = 2 })
    F.sfx(ev.sym == "." and "dot" or "dash")
  else
    cue.lamp_on = false
    if cue.kind == "rx" then
      orb_pin:set_style({ bg_color = C.mint_dim })
      orb_core:set_style({ bg_color = "#0c2a1d" })
      orb:set_style({ bg_color = "#0c2a1d", border_color = C.mint_deep })
      ring1:add_flag("HIDDEN")
      ring2:add_flag("HIDDEN")
      mode_dot:set_style({ opa = 150 })
    end
    card:set_style({ border_color = C.line, border_width = 1 })
  end
end

F.start_cue = function(code, kind)
  local events = {}
  local t = 0
  for i = 1, #code do
    local sym = code:sub(i, i)
    local dur = (sym == ".") and RX_UNIT or RX_UNIT * 3
    events[#events + 1] = { at = t, on = true, sym = sym, dur = dur }
    t = t + dur
    events[#events + 1] = { at = t, on = false }
    t = t + ((i < #code) and RX_UNIT or RX_UNIT * 3)
  end
  local now = system.millis()
  cue = {
    events = events, idx = 1, kind = kind,
    start_at = now + 180, total = t,
    lamp_on = false, lamp_start = now, lamp_dur = 1, lamp_sym = ".",
  }
  if kind == "rx" then
    caret:clear_flag("HIDDEN")
    caret:set_pos(22, 9)
    timer_fill:set_size(1, 8)
    timer_fill:set_style({ bg_color = C.amber })
    timer_color = C.amber
    timer_lbl:set_text("接收中")
    timer_text = "接收中"
  end
end

F.finish_cue = function(now)
  if not cue then
    return
  end
  local kind = cue.kind
  cue = nil
  card:set_style({ border_color = C.line, border_width = 1 })
  orb_pin:set_style({ bg_color = C.mint_dim })
  orb_core:set_style({ bg_color = "#0c2a1d" })
  orb:set_style({ bg_color = "#0c2a1d", border_color = C.mint_deep })
  ring1:add_flag("HIDDEN")
  ring2:add_flag("HIDDEN")
  caret:add_flag("HIDDEN")
  mode_dot:set_style({ opa = 255 })

  if kind == "rx" then
    state = "input"
    deadline = now + remaining_ms
    timer_w = WIRE_LEN
    timer_color = C.mint
    timer_fill:set_size(WIRE_LEN, 8)
    timer_fill:set_style({ bg_color = C.mint })
    local txt = string.format("%.1fs", remaining_ms / 1000)
    timer_lbl:set_text(txt)
    timer_text = txt
    if replays == 0 then
      aux_btn:set_text("重播 扣30%")
      aux_btn:clear_flag("HIDDEN")
    else
      aux_btn:add_flag("HIDDEN")
    end
    key:set_style({ opa = 255 })
    F.set_status("请抄收", C.cream)
    F.set_sub("按听到的节奏敲回", C.dim)
  else
    key:set_style({ opa = 255 })
    F.set_status("请发报", C.cream)
    F.set_sub("按目标字母敲出电码", C.dim)
  end
end

F.update_cue = function(now)
  if not cue then
    return
  end
  local t = now - cue.start_at
  while cue.idx <= #cue.events and t >= cue.events[cue.idx].at do
    F.apply_cue_event(cue.events[cue.idx], now)
    cue.idx = cue.idx + 1
    if not cue then
      return
    end
  end
  if t >= cue.total then
    F.finish_cue(now)
    return
  end
  local prog = F.clamp01(t / cue.total)
  if cue.kind == "rx" then
    caret:set_pos(22 + math.floor((208 - 2) * prog), 9)
    timer_fill:set_size(math.max(1, math.floor(WIRE_LEN * prog)), 8)
    if timer_color ~= C.amber then
      timer_fill:set_style({ bg_color = C.amber })
      timer_color = C.amber
    end
  end
  if cue.kind == "rx" and cue.lamp_on then
    local p = F.clamp01((now - cue.lamp_start) / cue.lamp_dur)
    local max_sz = (cue.lamp_sym == ".") and 96 or 132
    local sz1 = math.floor(56 + (max_sz - 56) * p)
    ring1:set_size(sz1, sz1)
    ring1:set_style({ opa = math.floor(230 * (1 - p)) })
    local p2 = F.clamp01((p - 0.15) / 0.85)
    local sz2 = math.floor(56 + (max_sz - 56) * p2)
    ring2:set_size(sz2, sz2)
    ring2:set_style({ opa = math.floor(180 * (1 - p2)) })
  end
end

-- ---------------------------------------------------------------- float / spark

F.start_float = function(text, color)
  float_lbl:set_text(text)
  float_lbl:set_style({ text_color = color, opa = 255 })
  float_lbl:set_pos(920, 240)
  float_lbl:clear_flag("HIDDEN")
  float_state.active = true
  float_state.t0 = system.millis()
end

F.start_spark = function(from_code, to_code)
  local a = tree_nodes[from_code]
  local b = tree_nodes[to_code]
  if not a or not b then
    return
  end
  spark_state.active = true
  spark_state.t0 = system.millis()
  spark_state.from_x = a.x
  spark_state.from_y = a.y
  spark_state.to_x = b.x
  spark_state.to_y = b.y
  spark:clear_flag("HIDDEN")
end

-- ---------------------------------------------------------------- key visuals

F.key_visual = function(down)
  local dy = down and 3 or 0
  lever:set_pos(200, 54 + dy)
  knob:set_pos(592, 32 + dy)
  key:set_style({
    border_color = down and C.brass or "#3a5a44",
    bg_color = down and "#11241a" or "#0a1510",
  })
end

F.reset_key = function()
  key_pressed = false
  key_latched = false
  key_latch_at = 0
  meter_visible = false
  meter_hide_at = 0
  meter_fill:add_flag("HIDDEN")
  F.key_visual(false)
end

-- ---------------------------------------------------------------- judging

F.judge_correct = function()
  local now = system.millis()
  state = "judge"
  judge_until = now + 1100
  F.reset_key()
  F.reset_lamp_visual()
  attempts = attempts + 1
  streak = streak + 1
  if streak > best_combo then
    best_combo = streak
  end

  local mult = 1 + math.min(streak, 10) * 0.1
  local remain = math.max(0, deadline - now)
  local ratio = (limit_ms > 0) and (remain / limit_ms) or 0
  local base = 60 + math.floor(120 * ratio)
  local factor = 1
  if mode == "tx" and hint_used then
    factor = factor * 0.7
  end
  if mode == "rx" and replays > 0 then
    factor = factor * 0.7
  end
  local pts = math.floor(base * mult * factor)
  score = score + pts
  correct_count = correct_count + 1

  F.update_score()
  F.update_combo()
  F.update_lives()
  F.update_hits()
  F.show_result(target_code, input_code, true)
  F.show_tape_code(target_code, C.mint)
  F.reveal_card()

  card_glow_state.active = true
  card_glow_state.t0 = now
  card_glow:set_style({ border_color = C.mint, opa = 220 })
  card_glow:set_pos(14, 38)
  card_glow:set_size(260, 154)
  card_glow:clear_flag("HIDDEN")

  local tn = tree_nodes[target_code]
  if tn then
    node_glow_state.active = true
    node_glow_state.t0 = now
    node_glow_state.cx = tn.x
    node_glow_state.cy = tn.y
    node_glow_state.base = tn.r * 2 + 10
    node_glow:set_style({ border_color = C.brass, opa = 220 })
    node_glow:set_pos(tn.x - node_glow_state.base // 2,
                      tn.y - node_glow_state.base // 2)
    node_glow:set_size(node_glow_state.base, node_glow_state.base)
    node_glow:clear_flag("HIDDEN")
  end

  F.start_float("+" .. tostring(pts), C.mint)
  F.set_status((mode == "tx" and "发报正确  +" or "抄收正确  +") .. pts, C.mint)
  F.set_sub(target_letter .. " = " .. F.pretty_code(target_code), C.cream)
  F.sfx("ok")
  aux_btn:add_flag("HIDDEN")
  key:set_style({ opa = 120 })
end

F.judge_wrong = function(reason)
  local now = system.millis()
  state = "judge"
  judge_until = now + 1500
  F.reset_key()
  F.reset_lamp_visual()
  attempts = attempts + 1
  lives = math.max(0, lives - 1)
  streak = 0

  F.update_combo()
  F.update_lives()
  F.update_hits()
  F.show_result(target_code, input_code, false)
  F.show_tape_code(target_code, C.amber)
  F.reveal_card()

  card_glow_state.active = true
  card_glow_state.t0 = now
  card_glow:set_style({ border_color = C.danger, opa = 220 })
  card_glow:set_pos(14, 38)
  card_glow:set_size(260, 154)
  card_glow:clear_flag("HIDDEN")

  local tn = tree_nodes[target_code]
  if tn then
    node_glow_state.active = true
    node_glow_state.t0 = now
    node_glow_state.cx = tn.x
    node_glow_state.cy = tn.y
    node_glow_state.base = tn.r * 2 + 10
    node_glow:set_style({ border_color = C.brass, opa = 220 })
    node_glow:set_pos(tn.x - node_glow_state.base // 2,
                      tn.y - node_glow_state.base // 2)
    node_glow:set_size(node_glow_state.base, node_glow_state.base)
    node_glow:clear_flag("HIDDEN")
  end

  F.set_status(reason, C.danger)
  F.set_sub(target_letter .. " = " .. F.pretty_code(target_code), C.amber)
  F.sfx("err")
  aux_btn:add_flag("HIDDEN")
  key:set_style({ opa = 120 })
end

F.add_input = function(sym)
  if state ~= "input" then
    return
  end
  if #input_code >= 4 then
    F.judge_wrong("电码有误")
    return
  end
  local from_code = input_code
  input_code = input_code .. sym
  F.set_pip(#input_code, sym, system.millis())
  F.show_live_path()
  F.start_spark(from_code, input_code)
  F.sfx(sym == "." and "dot" or "dash")
  if input_code == target_code then
    F.judge_correct()
  elseif target_code:sub(1, #input_code) ~= input_code then
    F.judge_wrong("电码有误")
  end
end

-- ---------------------------------------------------------------- key input

F.key_down = function()
  if state ~= "input" or key_pressed or key_latched then
    return
  end
  key_pressed = true
  key_press_at = system.millis()
  F.key_visual(true)
  F.sfx("key")
  meter_visible = true
  meter_hide_at = 0
  meter_w = 1
  meter_color = C.mint
  meter_fill:set_size(1, 3)
  meter_fill:set_style({ bg_color = C.mint })
  meter_fill:clear_flag("HIDDEN")
end

F.key_up = function()
  if not key_pressed then
    if key_latched then
      key_latched = false
      key_latch_at = 0
    end
    return
  end
  local now = system.millis()
  local held = now - key_press_at
  key_pressed = false
  F.key_visual(false)
  meter_hide_at = now + 280
  if state ~= "input" then
    return
  end
  F.add_input(held >= DASH_MS and "-" or ".")
end

-- ---------------------------------------------------------------- game flow

F.pick_target = function(level)
  local pool
  if level <= 1 then
    pool = POOL_1
  elseif level == 2 then
    pool = POOL_2
  else
    pool = ALL_LETTERS
  end
  local t
  local guard = 0
  repeat
    t = pool[math.random(#pool)]
    guard = guard + 1
  until t ~= last_target or guard > 12 or #pool == 1
  return t
end

F.start_round = function()
  local now = system.millis()
  local level = math.min(6, 1 + math.floor((round_no - 1) / 4))
  target_letter = F.pick_target(level)
  target_code = MORSE[target_letter]
  last_target = target_letter
  input_code = ""
  replays = 0
  hint_used = false
  cue = nil

  F.reset_key()
  F.reset_tree()
  F.clear_tape()
  F.reset_lamp_visual()
  F.update_combo()
  F.update_lives()
  F.update_hits()

  limit_ms = (mode == "tx")
             and math.max(5000, 12000 - (level - 1) * 1200)
             or math.max(4500, 10000 - (level - 1) * 1000)
  remaining_ms = limit_ms
  timer_w = WIRE_LEN
  timer_color = C.mint
  timer_fill:set_size(WIRE_LEN, 8)
  timer_fill:set_style({ bg_color = C.mint })
  local txt = string.format("%.1fs", limit_ms / 1000)
  timer_lbl:set_text(txt)
  timer_text = txt

  round_lbl:set_text(string.format("第 %02d 号电文　·　LV %d", round_no, level))
  card_glow:add_flag("HIDDEN")
  node_glow:add_flag("HIDDEN")
  card_glow_state.active = false
  node_glow_state.active = false
  float_lbl:add_flag("HIDDEN")
  float_state.active = false

  if mode == "tx" then
    state = "input"
    deadline = now + limit_ms
    card_mode:set_text("SEND")
    F.hide_orb()
    card_letter:set_text(target_letter)
    card_letter:set_style({ text_color = C.cream })
    card_letter:clear_flag("HIDDEN")
    key:set_style({ opa = 255 })
    aux_btn:set_text("提示 -30%")
    aux_btn:clear_flag("HIDDEN")
    F.set_status("请发报", C.cream)
    F.set_sub("按目标字母敲出电码", C.dim)
    mode_dot:set_style({ opa = 255 })
  else
    state = "receive"
    card_mode:set_text("RECV")
    F.show_orb()
    key:set_style({ opa = 120 })
    aux_btn:add_flag("HIDDEN")
    F.set_status("接收中…", C.amber)
    F.set_sub("短闪 = ·　长闪 = −", C.dim)
    F.start_cue(target_code, "rx")
  end
end

F.start_game = function(m)
  local now = system.millis()
  mode = m
  state = "intro"
  score = 0
  lives = MAX_LIVES
  round_no = 1
  streak = 0
  best_combo = 0
  correct_count = 0
  attempts = 0
  last_target = nil
  target_letter = ""
  target_code = ""
  input_code = ""
  new_record = false
  over_shown = false
  cue = nil

  F.reset_key()
  F.reset_tree()
  F.clear_tape()
  F.reset_lamp_visual()

  score_lbl:set_text(F.fmt5(0))
  F.update_combo()
  F.update_lives()
  F.update_hits()

  mode_lbl:set_text(m == "tx" and "发报训练" or "抄收挑战")
  mode_dot:set_style({
    bg_color = m == "tx" and C.mint or C.amber,
    opa = 255,
  })
  round_lbl:set_text("正在接入 · 7.023 MHz")

  card_mode:set_text("STBY")
  card_letter:add_flag("HIDDEN")
  F.show_orb()
  ring1:clear_flag("HIDDEN")
  ring2:clear_flag("HIDDEN")
  ring1:set_size(56, 56)
  ring2:set_size(56, 56)
  ring1:set_style({ opa = 180 })
  ring2:set_style({ opa = 120 })

  card_glow:add_flag("HIDDEN")
  node_glow:add_flag("HIDDEN")
  card_glow_state.active = false
  node_glow_state.active = false
  float_lbl:add_flag("HIDDEN")
  float_state.active = false

  key:set_style({ opa = 120 })
  aux_btn:add_flag("HIDDEN")
  menu_dim:add_flag("HIDDEN")
  over_dim:add_flag("HIDDEN")

  timer_w = WIRE_LEN
  timer_color = C.mint_dim
  timer_fill:set_size(WIRE_LEN, 8)
  timer_fill:set_style({ bg_color = C.mint_dim })
  timer_lbl:set_text("--")
  timer_text = "--"

  F.set_status("接入频率…", C.mint)
  F.set_sub("校准电键与信号灯", C.dim)

  intro_start = now
  intro_until = now + 900
  F.sfx("start")
end

F.game_over = function()
  local now = system.millis()
  state = "gameover"
  cue = nil
  F.reset_key()
  F.reset_lamp_visual()
  key:set_style({ opa = 120 })
  aux_btn:add_flag("HIDDEN")

  new_record = score > best
  if new_record then
    best = score
    local ok, f = pcall(io.open, BEST_PATH, "w")
    if ok and f then
      pcall(f.write, f, tostring(best))
      pcall(f.close, f)
    end
    best_lbl:set_text("最佳 " .. F.fmt5(best))
    menu_best:set_text("本台最高 " .. F.fmt5(best))
  end

  over_at = now + 650
  over_shown = false
  F.sfx("over")
end

F.show_menu = function()
  cue = nil
  state = "menu"
  F.reset_key()
  F.reset_lamp_visual()
  F.reset_tree()
  F.clear_tape()
  key:set_style({ opa = 120 })
  aux_btn:add_flag("HIDDEN")
  over_dim:add_flag("HIDDEN")
  menu_dim:clear_flag("HIDDEN")

  menu_best:set_text("本台最高 " .. F.fmt5(best))
  mode_lbl:set_text("待机")
  mode_dot:set_style({ bg_color = C.mint_dim, opa = 180 })
  round_lbl:set_text("选择发报或抄收")

  card_mode:set_text("STBY")
  card_letter:add_flag("HIDDEN")
  F.show_orb()
  ring1:add_flag("HIDDEN")
  ring2:add_flag("HIDDEN")
  card_glow:add_flag("HIDDEN")
  node_glow:add_flag("HIDDEN")
  card_glow_state.active = false
  node_glow_state.active = false
  float_lbl:add_flag("HIDDEN")
  float_state.active = false

  timer_w = WIRE_LEN
  timer_color = C.mint_dim
  timer_fill:set_size(WIRE_LEN, 8)
  timer_fill:set_style({ bg_color = C.mint_dim })
  timer_lbl:set_text("--")
  timer_text = "--"
  F.set_status("待机", C.muted)
  F.set_sub("选择模式开始通联", C.dim)
end

F.show_over = function()
  if over_shown then
    return
  end
  over_shown = true
  over_score:set_text(F.fmt5(score))
  local acc = (attempts > 0) and math.floor(correct_count * 100 / attempts) or 0
  over_stats:set_text(string.format(
    "命中 %d/%d　最高连击 %d　准确率 %d%%",
    correct_count, attempts, best_combo, acc))
  if new_record then
    over_record:clear_flag("HIDDEN")
  else
    over_record:add_flag("HIDDEN")
  end
  over_dim:clear_flag("HIDDEN")
  menu_dim:add_flag("HIDDEN")
end

F.start_hint = function()
  if state ~= "input" or mode ~= "tx" or cue then
    return
  end
  hint_used = true
  F.start_cue(target_code, "hint")
  aux_btn:add_flag("HIDDEN")
  F.set_status("提示播放中…", C.amber)
  F.set_sub("注意点与划的长短", C.dim)
end

F.start_replay = function()
  if state ~= "input" or mode ~= "rx" or cue then
    return
  end
  replays = replays + 1
  remaining_ms = math.max(1000, deadline - system.millis())
  input_code = ""
  F.clear_tape()
  F.reset_tree()
  F.reset_key()
  state = "receive"
  aux_btn:add_flag("HIDDEN")
  key:set_style({ opa = 120 })
  F.set_status("重播中…", C.amber)
  F.set_sub("短闪 = ·　长闪 = −", C.dim)
  F.start_cue(target_code, "rx")
end

-- ---------------------------------------------------------------- main loop

F.update = function(now)
  -- popup score
  if float_state.active then
    local t = (now - float_state.t0) / 700
    if t >= 1 then
      float_lbl:add_flag("HIDDEN")
      float_state.active = false
    else
      local e = 1 - (1 - t) * (1 - t)
      float_lbl:set_pos(920, math.floor(240 - 36 * e))
      float_lbl:set_style({ opa = math.floor(255 * (1 - t)) })
    end
  end

  -- tree spark
  if spark_state.active then
    local t = (now - spark_state.t0) / 180
    if t >= 1 then
      spark:add_flag("HIDDEN")
      spark_state.active = false
    else
      local e = 1 - (1 - t) * (1 - t) * (1 - t)
      local x = spark_state.from_x + (spark_state.to_x - spark_state.from_x) * e
      local y = spark_state.from_y + (spark_state.to_y - spark_state.from_y) * e
      spark:set_pos(math.floor(x) - 7, math.floor(y) - 7)
    end
  end

  -- result glow rings
  if card_glow_state.active then
    local t = (now - card_glow_state.t0) / 650
    if t >= 1 then
      card_glow:add_flag("HIDDEN")
      card_glow_state.active = false
    else
      local w = 260 + 24 * t
      local h = 154 + 24 * t
      card_glow:set_pos(math.floor(144 - w / 2), math.floor(115 - h / 2))
      card_glow:set_size(math.floor(w), math.floor(h))
      card_glow:set_style({ opa = math.floor(220 * (1 - t)) })
    end
  end

  if node_glow_state.active then
    local t = (now - node_glow_state.t0) / 650
    if t >= 1 then
      node_glow:add_flag("HIDDEN")
      node_glow_state.active = false
    else
      local sz = node_glow_state.base + 18 * t
      node_glow:set_pos(math.floor(node_glow_state.cx - sz / 2),
                        math.floor(node_glow_state.cy - sz / 2))
      node_glow:set_size(math.floor(sz), math.floor(sz))
      node_glow:set_style({ opa = math.floor(220 * (1 - t)) })
    end
  end

  -- pip flash settles to the quiet mint rim
  for i = 1, 4 do
    if pip_hot[i] and now >= pip_hide_at[i] then
      if pip_active[i] then
        pip_active[i]:set_style({ border_color = C.mint_dim })
      end
      pip_hot[i] = false
    end
  end

  -- menu sonar rings
  if state == "menu" then
    local p = (now % 2600) / 2600
    local a = 90 + math.floor(80 * (0.5 + 0.5 * math.sin(p * 6.283185307)))
    menu_ring1:set_style({ opa = a })
    menu_ring2:set_style({ opa = math.floor(a * 0.6) })
  end

  -- telegraph key hold meter
  if key_latched and now >= key_latch_at then
    key_latched = false
    key_latch_at = 0
  end
  if key_pressed then
    local held = now - key_press_at
    if state ~= "input" then
      F.reset_key()
    elseif held > 1600 then
      key_pressed = false
      key_latched = true
      F.key_visual(false)
      meter_fill:set_size(METER_W, 3)
      meter_fill:set_style({ bg_color = C.brass })
      meter_hide_at = now + 280
      F.add_input("-")
    else
      local p = math.min(1, held / HOLD_METER_MS)
      local w = math.max(1, math.floor(METER_W * p))
      if w ~= meter_w then
        meter_fill:set_size(w, 3)
        meter_w = w
      end
      local col = (held >= DASH_MS) and C.brass or C.mint
      if col ~= meter_color then
        meter_fill:set_style({ bg_color = col })
        meter_color = col
      end
    end
  elseif meter_visible and meter_hide_at > 0 and now >= meter_hide_at then
    meter_fill:add_flag("HIDDEN")
    meter_visible = false
    meter_hide_at = 0
  end

  if cue then
    F.update_cue(now)
  end

  if state == "intro" then
    local p = F.clamp01((now - intro_start) / 900)
    local sz = 56 + math.floor(64 * p)
    ring1:set_size(sz, sz)
    ring1:set_style({ opa = math.floor(180 * (1 - p)) })
    local p2 = F.clamp01((p - 0.2) / 0.8)
    local sz2 = 56 + math.floor(64 * p2)
    ring2:set_size(sz2, sz2)
    ring2:set_style({ opa = math.floor(140 * (1 - p2)) })
    if now >= intro_until then
      round_no = 1
      F.start_round()
    end
  elseif state == "input" then
    local remain = deadline - now
    if remain < 0 then
      remain = 0
    end
    local ratio = (limit_ms > 0) and (remain / limit_ms) or 0
    local w = math.max(1, math.floor(WIRE_LEN * ratio))
    if w ~= timer_w then
      timer_fill:set_size(w, 8)
      timer_w = w
    end
    local col = (ratio > 0.45) and C.mint
                or ((ratio > 0.2) and C.amber or C.danger)
    if col ~= timer_color then
      timer_fill:set_style({ bg_color = col })
      timer_color = col
    end
    local txt = string.format("%.1fs", remain / 1000)
    if txt ~= timer_text then
      timer_lbl:set_text(txt)
      timer_text = txt
    end
    if remain <= 0 then
      F.judge_wrong("信号窗口关闭")
    end
  elseif state == "judge" then
    if now >= judge_until then
      if lives <= 0 then
        F.game_over()
      else
        round_no = round_no + 1
        F.start_round()
      end
    end
  elseif state == "gameover" then
    if not over_shown and now >= over_at then
      F.show_over()
    end
  end
end

-- ---------------------------------------------------------------- events

key:on("pressed", F.key_down)
key:on("released", F.key_up)

aux_btn:on("clicked", function()
  if state ~= "input" then
    return
  end
  if mode == "tx" then
    F.start_hint()
  else
    F.start_replay()
  end
end)

menu_btn:on("clicked", function()
  if state ~= "menu" then
    F.show_menu()
  end
end)

mode_tx:on("clicked", function()
  F.start_game("tx")
end)
mode_rx:on("clicked", function()
  F.start_game("rx")
end)
over_again:on("clicked", function()
  F.start_game(mode)
end)
over_menu:on("clicked", function()
  F.show_menu()
end)

-- ---------------------------------------------------------------- start

F.show_menu()
scr:load()

while not claw.abort_requested() do
  local now = system.millis()
  F.update(now)
  lvgl.process_events(16)
end

-- Do not delete the fonts before deinit: live labels still point at them.
lvgl.deinit()
