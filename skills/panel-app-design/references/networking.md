# Networking from Lua Apps

This firmware build registers three network modules: `net`, `mqtt` and
`websocket`. They are ordinary globals, so `require("net")` and `net` are the
same table.

Verify availability with `GET /api/lua-modules` (or `probe_device.py`).
Firmware older than seed 28 does not have these modules; `require` then raises
"module not found".

## Runtime model

- Everything runs on the App's Lua job task. All blocking calls wait in 50 ms
  slices and re-check `claw.abort_requested()`, so the task list's "stop"
  ends a call within about 50 ms (TLS handshakes are the exception: they run on
  a helper task and are bounded by their timeout).
- Runtime failures return `nil, reason` (`"timeout"`, `"closed"`, `"stopped"`,
  `"dns failed"`, `"connect failed"`, `"socket budget exhausted ..."`).
  Wrong argument types raise a Lua error.
- Long-running network Apps must run as async jobs (they do when launched from
  the home page). The MCP `run_script` path has a short timeout.

## Socket budget

Per Lua job: 3 sockets, 1 TLS connection. Globally: 4 sockets, 2 TLS
connections. `mqtt`/`websocket` clients count as one socket each (plus TLS for
`mqtts://`/`wss://`). Exceeding the budget returns
`nil, "socket budget exhausted ..."`; `:close()` releases the slot immediately.
This keeps the Web UI and the voice assistant from being starved.

## net

```lua
net.online() / net.local_ip()
net.resolve("example.com", { timeout_ms = 3000, family = "A" }) -- one address

local s = net.tcp.connect("host", 80, { timeout_ms = 5000,
                                        tls = false, insecure = false,
                                        ca_pem = nil, server_name = nil })
s:send(data [, {timeout_ms=}])   s:send_all(data)
s:recv(max [, opts])             s:recv_exact(n)   s:recv_line()
s:wait_readable()                s:wait_writable()
s:set_timeout(ms)                s:set_nodelay(true)  s:set_keepalive()
s:local_addr() / s:remote()      s:shutdown("write")  s:is_open()  s:close()

local srv = net.tcp.listen(port_or_0); srv:local_addr(); srv:accept(); srv:close()

local u = net.udp.new({ port = 9001, broadcast = false, ttl = 64 })
u:send(data, host, port); u:recv(max, {timeout_ms=}); u:local_addr()
u:set_broadcast(); u:set_ttl(); u:join_multicast("239.1.2.3", {if_ip=})
u:close()

local r = net.http.request({ url="https://...", method="GET", headers={},
                             body=nil, timeout_ms=10000, max_body=65535,
                             follow_redirects=3, insecure=false, ca_pem=nil,
                             save_path=nil })
-- r.status, r.headers["content-type"], r.body / r.saved, r.truncated
```

`net.http` uses esp_http_client (TLS, chunked, redirects) with an abortable
read loop and a new connection per call (no keep-alive). `local` is a Lua
keyword, hence the method name `local_addr`.

Limitations: lwip returns one DNS address per lookup; IPv6 works only for
literal addresses; TLS is 1.2 with the built-in bundle or a script-supplied
PEM; do not bind port 80 (the Web UI owns it).

## mqtt / websocket (event stream)

Both use the same pattern: the network task only copies events into a queue,
the script pulls one event per `:next_event(ms)` call. There are no callbacks.

```lua
local m = mqtt.new({ uri="mqtt://broker:1883", client_id="panel-01",
                     will={topic="...", payload="offline", qos=1, retain=true},
                     reconnect_ms=0, max_payload=16384 })
m:connect(5000)                      -- true | nil, err
m:publish(t, payload, {qos=0, retain=false})
m:subscribe(t, 1); m:unsubscribe(t)
m:connected(); m:state(); m:last_error(); m:dropped()
local ev = m:next_event(50)          -- nil = timeout; 0 = non-blocking
-- ev.type = "connect" | "disconnect" | "message"{topic,payload,qos,truncated}
--          | "error"{message}
m:disconnect(); m:close()

local ws = websocket.connect("wss://host/path", { timeout_ms=5000,
                                                  reconnect_ms=5000,
                                                  max_payload=8192 })
ws:send("text"); ws:send(bytes, {binary=true})
local ev = ws:next_event(50)
-- ev.type = "connect" | "data"{data,binary,truncated} | "disconnect" | "error"
ws:close()
```

Subscriptions are re-applied on each reconnect (`"connect"` event). Oversized
payloads arrive truncated (`truncated=true`); a full queue drops the oldest
event and increments `:dropped()`.

## LAN server

`http_server` App routes are enabled in this build: `/lua/<app_id>/*` serves
`mount_static()` files and `/api/lua/<app_id>/*` dispatches to `app:get/post`
handlers. Handlers are invoked on the script task from `app:poll([ms])` (one
shot, up to 8 requests) or the blocking `app:serve_forever()`. Use `poll` when
the same loop also handles MQTT/WebSocket/LVGL:

```lua
local http_server = require("http_server")
local app = http_server.app("demo")
app:mount_static("assets")
app:get("/ping", function(req) return { body = "pong" } end)
-- inside the main loop:
app:poll(20)
```

The Web UI has no authentication; only use these modules on a trusted LAN and
only run trusted Apps (App Lua can open raw sockets and read SD).

## Example

`assets/examples/mqtt_panel/` is a complete App: MQTT heartbeat,
`color:#RRGGBB` / `text:` / `ping` commands, reconnect with status on screen.
See its README for the broker configuration and test commands.
