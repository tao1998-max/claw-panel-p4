# Lua MQTT

`require("mqtt")` (also the global `mqtt`) wraps esp-mqtt (MQTT 3.1.1) with an
event-stream API. The MQTT task only copies events into a queue; Lua consumes
them with `:next_event(ms)` on the script task, so there are no callbacks and
no cross-task Lua access.

Runtime failures return `nil, reason`; argument errors raise.

## Create

```lua
local m = mqtt.new({
  uri = "mqtt://broker:1883",        -- or mqtts://
  client_id = "panel-01",
  username = nil, password = nil,
  keepalive_s = 60,                  -- 0 disables keep-alive
  clean_session = true,
  will = { topic = "dev/01/offline", payload = "offline", qos = 1, retain = true },
  reconnect_ms = 10000,              -- 0 = no automatic reconnect
  insecure = false,                  -- mqtts only: skip verification
  ca_pem = nil,                      -- mqtts only: custom CA instead of bundle
  max_payload = 16384,               -- clamp 512..65536
})
```

One MQTT client per job, two globally. A client holds one socket (and one TLS
slot for `mqtts://`) from `new()` until `close()`.

## Use

```lua
m:connect(5000)                        -- waits for CONNECTED; true or nil, err
m:publish("dev/01/state", "online", { qos = 1, retain = true })
m:subscribe("dev/01/cmd", 1)
m:unsubscribe("dev/01/cmd")
m:connected()                          -- bool
m:state() / m:last_error() / m:dropped()

local ev = m:next_event(50)            -- nil = timeout; 0 = non-blocking
-- ev.type = "connect"
--          | "disconnect"
--          | "message"    { topic=, payload=, qos=, truncated= }
--          | "error"      { message= }
```

Subscriptions are remembered and re-applied when a reconnect produces a new
`"connect"` event. Messages larger than `max_payload` arrive truncated with
`truncated=true`; a full event queue drops the oldest event and increments
`m:dropped()`.

`m:disconnect()` stops the client but keeps the handle; `m:close()` stops,
destroys and releases the socket budget. Always close (or let the job end) —
a job that returns without closing still runs `__gc`, but explicit close frees
the socket earlier.

## Example loop

```lua
local m = mqtt.new({ uri = "mqtt://192.168.1.10:1883", client_id = "panel-01" })
if not m:connect(5000) then return end
m:subscribe("panel/cmd", 1)

while not claw.abort_requested() do
  local ev = m:next_event(50)
  if ev and ev.type == "message" then
    print(ev.topic, ev.payload)
  end
  lvgl.process_events(20)
end
m:close()
```

`connect()` waits in 50 ms slices and honours the job stop request. A failed
initial connect with `reconnect_ms = 0` stops the client; with auto-reconnect
the library keeps retrying and later `"connect"` events are surfaced by
`next_event`.
