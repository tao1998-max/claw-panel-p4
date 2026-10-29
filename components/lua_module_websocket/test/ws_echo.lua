-- WebSocket echo test.
--
-- args:
--   url  ws:// or wss:// echo endpoint; without it the test prints SKIP
--        (for a local check: python -m websockets ws://0.0.0.0:9002 --echo)

local websocket = require("websocket")
local system = require("system")

local a = type(args) == "table" and args or {}
local url = a.url

if type(url) ~= "string" or url == "" then
    print("[ws_echo] SKIP: pass args.url (e.g. ws://192.168.1.10:9002)")
    return
end

local function check(name, ok, detail)
    print(string.format("[ws_echo] %s %s%s",
                        ok and "PASS" or "FAIL", name,
                        detail and (": " .. tostring(detail)) or ""))
    return ok
end

local ws, err = websocket.connect(url, { timeout_ms = 8000, max_payload = 4096 })
if not check("connect", ws ~= nil, err) then
    return
end

check("send", ws:send("hello-ws") ~= nil)

local got = false
local deadline = system.millis() + 6000
while system.millis() < deadline do
    local ev = ws:next_event(200)
    if ev and ev.type == "data" and ev.data == "hello-ws" then
        got = true
        break
    end
end
check("echo", got, got and nil or ws:last_error())

ws:close()
print("[ws_echo] done")
