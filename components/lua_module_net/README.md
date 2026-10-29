# Lua net (TCP / UDP / DNS / HTTP)

`require("net")` (also available as the global `net`) gives Lua apps blocking
sockets with deadlines, DNS lookups, and an HTTP client.

All waits are sliced (50 ms) and check the job's stop request, so
`lua_stop_async_job` / the task list's "stop" button interrupt a call within
about 50 ms. Errors that happen at runtime are returned as `nil, reason`
(`"timeout"`, `"closed"`, `"stopped"`, `"dns failed"`, ...); wrong argument
types raise a Lua error.

## Budget

At most **3 sockets per job** and **4 globally** may be open at once (TLS
connections also count against **2 globally**). Exceeding the budget returns
`nil, "socket budget exhausted ..."`. Close handles when done; every
`:close()` releases the slot immediately.

## DNS and interface state

```lua
local ip = net.resolve("example.com", { timeout_ms = 3000 })        -- IPv4
local ip6 = net.resolve("example.com", { family = "AAAA" })         -- IPv6
net.online()      -- true when Wi-Fi STA or Ethernet has an IPv4 address
net.local_ip()    -- "192.168.1.79" or nil
```

lwip exposes one address per lookup (`DNS_MAX_HOST_IP=1`); IPv6 works only for
literal addresses in practice (SLAAC is disabled on this product).

## TCP

```lua
local s = net.tcp.connect("host", 80, {
  timeout_ms = 5000,
  tls = false,              -- true: TLS with the built-in CA bundle
  insecure = false,         -- true: skip certificate verification
  ca_pem = nil,             -- custom CA (PEM string) instead of the bundle
  server_name = nil,        -- SNI/CN override
})
```

Connection methods:

| Method | Meaning |
| --- | --- |
| `s:send(data [, opts])` | Write once; returns bytes written (may be partial) or `nil, err` |
| `s:send_all(data [, opts])` | Write until everything is sent; `true` or `nil, err` |
| `s:recv(max [, opts])` | Up to `max` bytes (default/cap 64 KiB); `nil, "timeout"` / `nil, "closed"` |
| `s:recv_exact(n [, opts])` | Exactly `n` bytes or `nil, err` |
| `s:recv_line([opts])` | One line including `\n` (max 1024 bytes) |
| `s:wait_readable([opts])` / `s:wait_writable([opts])` | `true`/`false` (false = timeout) |
| `s:set_timeout(ms)` | Default timeout for later calls |
| `s:set_nodelay(bool)` / `s:set_keepalive({idle_s, interval_s})` | Plain TCP only |
| `s:local()` / `s:remote()` | `{ip=, port=}` tables |
| `s:shutdown("read"\|"write"\|"both")` | Half close (plain TCP only) |
| `s:is_open()`, `s:close()` | |

`opts` is `{ timeout_ms = ... }`.

Raw TLS is performed on a helper task (the Lua job stack is small), so a
handshake can take up to its timeout before a stop request is noticed.
`local()`/`remote()`/`set_nodelay`/`shutdown` return
`nil, "not supported for TLS connections"`.

Listener (small scale, tests):

```lua
local srv = net.tcp.listen(18080, { backlog = 4 })  -- port 0 = ephemeral
local addr = srv:local_addr()                            -- {ip=, port=}
local c = srv:accept({ timeout_ms = 5000 })
srv:close()
```

## UDP

```lua
local u = net.udp.new({ port = 9001, broadcast = true, ttl = 64 })
u:send("ping", "192.168.1.50", 9000)
local data, ip, port = u:recv(1500, { timeout_ms = 2000 })
u:set_broadcast(true) ; u:set_ttl(32)
u:join_multicast("239.255.255.250", { if_ip = "192.168.1.79" })
u:local_addr()   -- {ip=, port=}
u:close()
```

## HTTP

```lua
local r = net.http.request({
  url = "https://api.example.com/v1/status",
  method = "GET",                 -- GET/POST/PUT/PATCH/DELETE/HEAD
  headers = { ["Authorization"] = "Bearer ..." },
  body = nil,
  timeout_ms = 10000,
  max_body = 65535,               -- cap 1 MiB
  follow_redirects = 3,
  insecure = false,
  ca_pem = nil,
  save_path = nil,                -- stream the body to a file instead
})
-- r.status, r.headers["content-type"], r.body, r.truncated
-- with save_path: r.saved = save_path, no r.body
```

Implemented on `esp_http_client` (TLS, chunked decoding and redirects are
handled there) with the abortable open/read loop from `claw_http`. Every call
uses a new connection (no keep-alive). Response headers are collected up to
16 entries / 8 KiB.

## Notes

- Wi-Fi may come up after a script starts; check `net.online()` and retry.
- Do not bind port 80 (the Web UI owns it).
- Long-running servers must run as async jobs; the MCP `run_script` path is
  capped at a few seconds.
