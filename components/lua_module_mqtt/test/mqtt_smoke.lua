-- MQTT smoke test: connect, subscribe, publish and wait for the echo.
--
-- args:
--   broker  mqtt URI (mqtt://host:port or mqtts://host:8883); without it the
--           test prints SKIP
--   topic   base topic (default "lua/mqtt_smoke/<millis>")
--
-- The broker must deliver a client's own published message back to it (all
-- mainstream brokers do).

local mqtt = require("mqtt")
local system = require("system")

local a = type(args) == "table" and args or {}
local broker = a.broker
local topic = a.topic or ("lua/mqtt_smoke/" .. tostring(system.millis()))

if type(broker) ~= "string" or broker == "" then
    print("[mqtt_smoke] SKIP: pass args.broker (e.g. mqtt://192.168.1.10:1883)")
    return
end

local function check(name, ok, detail)
    print(string.format("[mqtt_smoke] %s %s%s",
                        ok and "PASS" or "FAIL", name,
                        detail and (": " .. tostring(detail)) or ""))
    return ok
end

local m, err = mqtt.new({
    uri = broker,
    client_id = "mqtt-smoke-" .. tostring(system.millis()),
    reconnect_ms = 0,
    max_payload = 4096,
})
if not check("new", m ~= nil, err) then
    return
end

local ok, cerr = m:connect(8000)
if not check("connect", ok, cerr) then
    m:close()
    return
end

check("subscribe", m:subscribe(topic, 1))
check("publish", m:publish(topic, "hello", { qos = 1 }))

local got = false
local deadline = system.millis() + 8000
while system.millis() < deadline do
    local ev = m:next_event(200)
    if ev and ev.type == "message" and ev.payload == "hello" then
        got = true
        break
    end
end
check("echo", got, got and nil or m:last_error())

m:close()
print("[mqtt_smoke] done")
