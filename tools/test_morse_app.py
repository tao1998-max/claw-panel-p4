#!/usr/bin/env python3
"""Run apps/morse_station/scripts/main.lua against a stub LVGL runtime.

This is a logic/smoke test, not a renderer.  It fakes the Lua modules the App
uses, drives the menu / telegraph key / receive cue, and checks the state the
script reaches.  Run with:

    py -3 tools/test_morse_app.py
"""

from __future__ import annotations

import sys
from pathlib import Path

try:
    import lupa
except ImportError:
    sys.exit("lupa is required: pip install lupa")

ROOT = Path(__file__).resolve().parent.parent
APP = ROOT / "apps" / "morse_station" / "scripts" / "main.lua"

MOCK_LUA = r"""
MOCK = {
  clock = 0, iter = 0, objects = {}, abort = false,
  sfx = {}, log = {}, collect = nil, errors = {},
  phase = "boot", timeouts = 0,
}

local function push(t, v) t[#t + 1] = v end

function MOCK.note(msg)
  push(MOCK.log, msg)
end

local real_open = io.open
io.open = function(path, mode)
  if type(path) == "string" and path:sub(1, 1) == "/" then
    return nil, "mock"
  end
  return real_open(path, mode)
end

local METHODS = {}

function METHODS:set_style(t)
  for k, v in pairs(t) do
    self.style[k] = v
  end
end

function METHODS:set_pos(x, y)
  self.x, self.y = x, y
end

function METHODS:get_pos()
  return self.x, self.y
end

function METHODS:set_size(w, h)
  self.w, self.h = w, h
end

function METHODS:get_size()
  return self.w, self.h
end

function METHODS:align(...) end
function METHODS:set_flex(...) end
function METHODS:set_grid(...) end
function METHODS:set_grid_cell(...) end
function METHODS:set_scroll(...) end
function METHODS:move_foreground() end
function METHODS:delete() end
function METHODS:clean() end
function METHODS:is_valid() return true end
function METHODS:load() end
function METHODS:set_rotation(...) end
function METHODS:set_pivot(...) end
function METHODS:set_points(...) end
function METHODS:set_value(...) end
function METHODS:get_value() return 0 end
function METHODS:set_range(...) end
function METHODS:set_raw_rgb565(...) end
function METHODS:set_raw_argb8888(...) end

function METHODS:add_flag(flag)
  self.flags[flag] = true
end

function METHODS:clear_flag(flag)
  self.flags[flag] = nil
end

function METHODS:set_text(text)
  self.text = text
end

function METHODS:on(event, fn)
  self.events[event] = fn
end

function METHODS:off(...) end

local STYLE_KEYS = {
  bg_color = true, text_color = true, border_color = true, line_color = true,
  bg_opa = true, opa = true, radius = true, border_width = true,
  pad = true, pad_row = true, pad_column = true, line_width = true,
  arc_width = true,
}

local function new_obj(kind, parent, opts)
  opts = opts or {}
  local o = setmetatable({
    kind = kind, parent = parent, opts = opts,
    x = opts.x or 0, y = opts.y or 0, w = opts.w or 0, h = opts.h or 0,
    text = opts.text, font = opts.font,
    style = {}, flags = {}, events = {},
  }, {
    __index = function(_, key)
      return METHODS[key]
    end,
  })
  for k, v in pairs(opts) do
    if STYLE_KEYS[k] then
      o.style[k] = v
    end
  end
  if opts.font then
    o.style.font = opts.font
  end
  push(MOCK.objects, o)
  return o
end

local LVGL = {}
function LVGL.init(opts) MOCK.init = opts end
function LVGL.create_screen() return new_obj("screen", nil, {}) end
function LVGL.label(parent, opts) return new_obj("label", parent, opts) end
function LVGL.button(parent, opts) return new_obj("button", parent, opts) end
function LVGL.container(parent, opts) return new_obj("container", parent, opts) end
function LVGL.image(parent, opts) return new_obj("image", parent, opts) end
function LVGL.line(parent, opts) return new_obj("line", parent, opts) end
function LVGL.font_load(path, opts)
  return { size = opts and opts.size or 20, path = path }
end
function LVGL.set_default_font(font) MOCK.default_font = font end
function LVGL.deinit() MOCK.deinited = true end

function LVGL.process_events(ms)
  MOCK.clock = MOCK.clock + (ms or 0)
  MOCK.iter = MOCK.iter + 1
  if MOCK.iter > 6000 then
    MOCK.note("hard timeout")
    MOCK.abort = true
    return
  end
  local ok, err = pcall(MOCK.tick)
  if not ok then
    push(MOCK.errors, "tick: " .. tostring(err))
    MOCK.abort = true
  end
end

local IMAGE = {
  RGB565 = 1,
  load_file = function(path) return nil end,
  convert = function(frame, fmt) return nil end,
}

local AUDIO = {
  play = function(name)
    push(MOCK.sfx, name)
    if MOCK.collect and (name == "dot" or name == "dash") then
      push(MOCK.collect, name == "dot" and "." or "-")
    end
    return true
  end,
  is_available = function() return true end,
}

local SYSTEM = {
  millis = function() return MOCK.clock end,
}

package.preload["lvgl"] = function() return LVGL end
package.preload["image"] = function() return IMAGE end
package.preload["audio"] = function() return AUDIO end
package.preload["system"] = function() return SYSTEM end

claw = {
  abort_requested = function() return MOCK.abort end,
}

MOCK.MORSE = {
  A=".-", B="-...", C="-.-.", D="-..", E=".", F="..-.", G="--.", H="....",
  I="..", J=".---", K="-.-", L=".-..", M="--", N="-.", O="---", P=".--.",
  Q="--.-", R=".-.", S="...", T="-", U="..-", V="...-", W=".--", X="-..-",
  Y="-.--", Z="--..",
}

function MOCK.find(pred)
  for _, o in ipairs(MOCK.objects) do
    if pred(o) then return o end
  end
  return nil
end

function MOCK.find_by(kind, props)
  return MOCK.find(function(o)
    if kind and o.kind ~= kind then return false end
    for k, v in pairs(props) do
      if o[k] ~= v then return false end
    end
    return true
  end)
end

function MOCK.find_text(text)
  return MOCK.find(function(o) return o.text == text end)
end

function MOCK.find_key()
  return MOCK.find(function(o)
    return o.kind == "button" and o.w == 680 and o.h == 84
  end)
end

function MOCK.target_letter()
  local o = MOCK.find(function(o)
    return o.kind == "label" and o.font and o.font.size == 96
       and o.opts and o.opts.align == "center"
       and not o.flags.HIDDEN and o.text and o.text:match("^[A-Z]$")
  end)
  return o and o.text or nil
end

function MOCK.status()
  local o = MOCK.find(function(o)
    return o.kind == "label" and o.opts and o.opts.align == "top_mid"
       and o.opts.y == 270
  end)
  return o and o.text or ""
end

function MOCK.roundtext()
  local o = MOCK.find(function(o)
    return o.kind == "label" and o.x == 404 and o.y == 42
  end)
  return o and o.text or ""
end

function MOCK.dim_with_card(cw, ch)
  return MOCK.find(function(o)
    if o.kind ~= "container" or o.w ~= 1024 or o.h ~= 600 then return false end
    return MOCK.find(function(c)
      return c.parent == o and c.kind == "container"
         and c.w == cw and c.h == ch
    end) ~= nil
  end)
end

function MOCK.over_visible()
  local o = MOCK.dim_with_card(540, 392)
  return o and not o.flags.HIDDEN or false
end

function MOCK.fire(obj, event)
  if not obj then
    push(MOCK.errors, "missing object for " .. event)
    MOCK.abort = true
    return
  end
  local fn = obj.events[event]
  if not fn then
    push(MOCK.errors, "missing handler " .. event)
    MOCK.abort = true
    return
  end
  local ok, err = pcall(fn)
  if not ok then
    push(MOCK.errors, event .. ": " .. tostring(err))
    MOCK.abort = true
  end
end

function MOCK.send_code(code)
  local key = MOCK.find_key()
  for i = 1, #code do
    local sym = code:sub(i, i)
    MOCK.fire(key, "pressed")
    MOCK.clock = MOCK.clock + ((sym == ".") and 100 or 400)
    MOCK.fire(key, "released")
  end
end

function MOCK.tick()
  local st = MOCK.status()
  local phase = MOCK.phase

  if phase == "boot" then
    if MOCK.iter >= 3 then
      MOCK.fire(MOCK.find_by("button", { w = 292, h = 148, x = 40 }), "clicked")
      MOCK.phase = "tx_wait"
      MOCK.note("started TX")
    end

  elseif phase == "tx_wait" then
    local letter = MOCK.target_letter()
    local rt = MOCK.roundtext()
    if letter and rt:match("第 01") then
      MOCK.note("TX target " .. letter)
      MOCK.send_code(MOCK.MORSE[letter])
      MOCK.phase = "tx_after_correct"
    end

  elseif phase == "tx_after_correct" then
    local rt = MOCK.roundtext()
    if rt:match("第 02") then
      local letter = MOCK.target_letter()
      if letter then
        local first = MOCK.MORSE[letter]:sub(1, 1)
        local wrong = (first == ".") and "-" or "."
        MOCK.note("TX round 2 target " .. letter .. ", sending wrong " .. wrong)
        local key = MOCK.find_key()
        MOCK.fire(key, "pressed")
        MOCK.clock = MOCK.clock + ((wrong == ".") and 100 or 400)
        MOCK.fire(key, "released")
        MOCK.phase = "tx_after_wrong"
      end
    end

  elseif phase == "tx_after_wrong" then
    if MOCK.roundtext():match("第 03") then
      MOCK.fire(MOCK.find_text("菜单"), "clicked")
      MOCK.phase = "menu"
      MOCK.note("returned to menu")
    end

  elseif phase == "menu" then
    if MOCK.find_by("button", { w = 292, h = 148, x = 348 }) then
      MOCK.fire(MOCK.find_by("button", { w = 292, h = 148, x = 348 }), "clicked")
      MOCK.phase = "rx_wait"
      MOCK.note("started RX")
    end

  elseif phase == "rx_wait" then
    if st == "接收中…" then
      MOCK.collect = {}
      MOCK.phase = "rx_cue"
    end

  elseif phase == "rx_cue" then
    if st == "请抄收" then
      MOCK.phase = "rx_replay"
      local code = table.concat(MOCK.collect)
      MOCK.note("RX heard " .. code)
      MOCK.collect = {}
      MOCK.fire(MOCK.find_text("重播 扣30%"), "clicked")
    end

  elseif phase == "rx_replay" then
    if st == "重播中…" then
      MOCK.replay_seen = true
    elseif st == "请抄收" and MOCK.replay_seen then
      local code = table.concat(MOCK.collect)
      MOCK.note("RX replay heard " .. code)
      MOCK.collect = nil
      MOCK.send_code(code)
      MOCK.phase = "rx_after_correct"
    end

  elseif phase == "rx_after_correct" then
    if st == "接收中…" then
      MOCK.phase = "rx_timeout1"
      MOCK.note("round 5 cue, waiting for timeout")
    end

  elseif phase == "rx_timeout1" then
    if st == "信号窗口关闭" then
      MOCK.timeouts = MOCK.timeouts + 1
      MOCK.note("timeout 1")
      MOCK.phase = "rx_timeout2_wait"
    end

  elseif phase == "rx_timeout2_wait" then
    if st == "接收中…" then
      MOCK.phase = "rx_timeout2"
      MOCK.note("round 6 cue, waiting for timeout")
    end

  elseif phase == "rx_timeout2" then
    if st == "信号窗口关闭" then
      MOCK.timeouts = MOCK.timeouts + 1
      MOCK.note("timeout 2")
      MOCK.phase = "over_wait"
    end

  elseif phase == "over_wait" then
    if MOCK.over_visible() then
      MOCK.note("game over overlay visible")
      MOCK.fire(MOCK.find_text("返回菜单"), "clicked")
      MOCK.phase = "menu_after_over"
      MOCK.over_check_at = MOCK.iter + 20
    end

  elseif phase == "menu_after_over" then
    if MOCK.iter >= MOCK.over_check_at then
      MOCK.abort = true
      MOCK.phase = "done"
    end
  end
end

function MOCK.summary()
  local score_obj = MOCK.find(function(o)
    return o.kind == "label" and o.x == 680 and o.y == 22
  end)
  local lives = 0
  for _, o in ipairs(MOCK.objects) do
    if o.kind == "container" and o.w == 18 and o.h == 18
       and o.style.bg_color == "#6ff0b8" then
      lives = lives + 1
    end
  end
  local menu_visible = false
  local m = MOCK.dim_with_card(680, 472)
  if m then menu_visible = not m.flags.HIDDEN end
  return string.format(
    "iter=%d score=%s lives=%d menu=%s over=%s phase=%s timeouts=%d",
    MOCK.iter, score_obj and score_obj.text or "?",
    lives, tostring(menu_visible), tostring(MOCK.over_visible()),
    MOCK.phase, MOCK.timeouts)
end

function MOCK.logtext()
  return table.concat(MOCK.log, " | ")
end

function MOCK.sfxtext()
  return table.concat(MOCK.sfx, ",")
end
"""


def main() -> int:
    source = APP.read_text(encoding="utf-8")
    lua = lupa.LuaRuntime(unpack_returned_tuples=True)
    lua.execute(MOCK_LUA)

    chunk_name = "@apps/morse_station/scripts/main.lua"
    try:
        chunk = lua.compile(source, chunk_name)
        chunk()
    except lupa.LuaError as exc:
        print("LUA ERROR:", exc)
        return 1

    mock = lua.globals().MOCK
    print("summary:", mock.summary())
    print("log:", mock.logtext())
    print("sfx:", mock.sfxtext())
    errors = mock.errors
    errs = []
    if errors is not None:
        for value in errors.values():
            errs.append(str(value))
    if errs:
        print("errors:", " | ".join(errs))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
