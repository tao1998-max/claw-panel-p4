-- MQTT panel example: publishes a status heartbeat and renders commands.
--
-- launcher.json args:
--   broker      MQTT URI, e.g. "mqtt://192.168.1.10:1883" (mqtts:// works too)
--   base_topic  topic prefix; the app uses <base>/status and <base>/cmd
--
-- Commands (payload strings on <base>/cmd):
--   "color:#RRGGBB"  change the background colour
--   "text:..."       show a line of text
--   "ping"           answers on <base>/status> with "pong"

local lvgl = require("lvgl")
local system = require("system")
local mqtt = require("mqtt")

local broker = (args and args.broker) or "mqtt://192.168.0.107:1884"
local base = (args and args.base_topic) or "panel/demo"
local status_topic = base .. "/status"
local cmd_topic = base .. "/cmd"

local bg = "#0b1220"
local accent = "#7b88ff"
local ok_color = "#3ddc97"
local err_color = "#ff6b81"

lvgl.init({ buffer_lines = 40, tick_ms = 5, task_period_ms = 10 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = bg, pad = 0 })

lvgl.label(scr, { text = "MQTT PANEL", x = 40, y = 32, text_color = accent })
local status_lbl = lvgl.label(scr, { text = "connecting...", x = 40, y = 96,
                                     text_color = "#93a0b8" })
local topic_lbl = lvgl.label(scr, { text = broker, x = 40, y = 132,
                                    text_color = "#5c6a84" })
local message_lbl = lvgl.label(scr, { text = "(no command yet)", x = 40, y = 220,
                                     text_color = "#f4f7fc" })
local hint_lbl = lvgl.label(scr, { text = "send 'color:#RRGGBB', 'text:...' or 'ping' to " .. cmd_topic,
                                   x = 40, y = 520, text_color = "#5c6a84" })

scr:load()

local client = nil
local connected = false
local last_connect_attempt = 0
local last_publish = 0

local function set_status(text, color)
  status_lbl:set_text(text)
  status_lbl:set_style({ text_color = color })
end

local function disconnect_client()
  if client then
    client:close()
    client = nil
  end
  connected = false
end

local function connect_client()
  disconnect_client()
  set_status("connecting...", accent)
  local c, err = mqtt.new({
    uri = broker,
    client_id = "panel-demo-" .. tostring(system.millis()),
    reconnect_ms = 0,
    max_payload = 4096,
  })
  if not c then
    set_status("init failed: " .. tostring(err), err_color)
    return
  end
  local ok, cerr = c:connect(8000)
  if not ok then
    set_status("connect failed: " .. tostring(cerr), err_color)
    c:close()
    return
  end
  client = c
  connected = true
  set_status("connected", ok_color)
  client:subscribe(cmd_topic, 1)
  client:publish(status_topic, "online", { qos = 1, retain = true })
end

local function handle_message(payload)
  message_lbl:set_text(payload)
  if payload:sub(1, 6) == "color:" then
    scr:set_style({ bg_color = payload:sub(7) })
  elseif payload == "ping" and client then
    client:publish(status_topic, "pong", { qos = 0 })
  end
end

while not claw.abort_requested() do
  local now = system.millis()

  if not connected and now - last_connect_attempt > 5000 then
    last_connect_attempt = now
    connect_client()
  end

  if connected and client then
    local ev = client:next_event(50)
    if ev then
      if ev.type == "message" then
        handle_message(ev.payload or "")
      elseif ev.type == "disconnect" then
        connected = false
        set_status("disconnected, retrying", err_color)
      elseif ev.type == "error" then
        set_status("error: " .. tostring(ev.message), err_color)
      end
    end
    if now - last_publish > 10000 then
      last_publish = now
      client:publish(status_topic, "alive", { qos = 0 })
    end
  end

  lvgl.process_events(20)
end

disconnect_client()
lvgl.deinit()
