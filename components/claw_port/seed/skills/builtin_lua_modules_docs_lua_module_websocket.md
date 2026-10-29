# Lua WebSocket

`require("websocket")` (also the global `websocket`) wraps
`espressif__esp_websocket_client` with an event-stream API. The websocket task
only copies (reassembled) messages into a queue; Lua consumes them with
`:next_event(ms)` on the script task. No callbacks, no cross-task Lua access.

## Connect

```lua
local ws = websocket.connect("wss://host/path", {
  headers = "X-Token: abc\r\n",       -- raw extra headers string
  subprotocol = nil,
  timeout_ms = 5000,                  -- connect and per-operation timeout
  insecure = false,                   -- wss only: skip verification
  ca_pem = nil,                       -- wss only: custom CA
  reconnect_ms = 5000,                -- 0 = no automatic reconnect
  ping_interval_s = 10,               -- 0 disables client pings
  pingpong_timeout_s = 15,
  buffer_size = 2048,                 -- 512..16384
  task_stack = 6144,                  -- 4096..32768
  max_payload = 8192,                 -- clamp 512..65536
})
```

`connect` blocks until the handshake succeeds (50 ms slices, stop-aware) and
returns the client, or `nil, reason` (`"timeout"`, `"stopped"`, ...). One
client per job, two globally; it holds one socket (plus a TLS slot for `wss`).

## Use

```lua
ws:send("hello")                      -- text
ws:send(bytes, { binary = true, timeout_ms = 3000 })
ws:connected()                        -- bool
ws:last_error() / ws:dropped()

local ev = ws:next_event(50)          -- nil = timeout; 0 = non-blocking
-- ev.type = "connect"
--          | "data"        { data=, binary=, truncated= }
--          | "disconnect"
--          | "error"       { message= }

ws:close()                            -- sends a close frame, stops, frees budget
```

Fragmented messages are reassembled (`payload_offset`/`payload_len`) before an
event is queued. Messages larger than `max_payload` arrive truncated with
`truncated=true`; a full event queue drops the oldest event and increments
`ws:dropped()`. Control frames (ping/pong/close) are handled by the client.

With `reconnect_ms > 0` the client reconnects automatically; each successful
reconnect produces another `"connect"` event from `next_event`.

## Example

```lua
local ws = websocket.connect("ws://192.168.1.10:9002", { timeout_ms = 5000 })
if not ws then return end
ws:send("hello from panel")

while not claw.abort_requested() do
  local ev = ws:next_event(50)
  if ev and ev.type == "data" then
    print(ev.data)
  end
  lvgl.process_events(20)
end
ws:close()
```
