-- net loopback self-test: UDP round-trip, TCP listen/connect round-trip and the
-- per-job socket budget. No network or SD card required.
--
-- Run from the Claw page or `lua_run_script_async`; every check prints one line.

local net = require("net")

local failures = 0

local function check(name, ok, detail)
    if not ok then
        failures = failures + 1
    end
    print(string.format("[net_loopback] %s %s%s",
                        ok and "PASS" or "FAIL", name,
                        detail and (": " .. tostring(detail)) or ""))
end

-- UDP: send to our own bound port.
do
    local u = net.udp.new({ port = 0 })
    local ok = false
    if u then
        local l = u:local_addr()
        if l and u:send("ping", "127.0.0.1", l.port) then
            ok = u:recv(64, { timeout_ms = 2000 }) == "ping"
        end
        u:close()
    end
    check("udp_roundtrip", ok)
end

-- TCP: listener + client in the same job (accept after connect hits the
-- backlog, so no second job is needed).
do
    local srv = net.tcp.listen(0, { backlog = 1 })
    local ok = false
    if srv then
        local l = srv:local_addr()
        local c = net.tcp.connect("127.0.0.1", l.port, { timeout_ms = 2000 })
        local peer = c and srv:accept({ timeout_ms = 2000 })
        if c and peer then
            c:send_all("hello\n")
            local line = peer:recv_line({ timeout_ms = 2000 })
            peer:send_all("world\n")
            local back = c:recv_line({ timeout_ms = 2000 })
            ok = (line == "hello\n") and (back == "world\n")
            peer:close()
        end
        if c then c:close() end
        srv:close()
    end
    check("tcp_roundtrip", ok)
end

-- Budget: the fourth socket in one job must be rejected.
do
    local open = {}
    local rejected = nil
    for _ = 1, 5 do
        local srv, err = net.tcp.listen(0)
        if srv then
            open[#open + 1] = srv
        else
            rejected = err
            break
        end
    end
    for _, srv in ipairs(open) do
        srv:close()
    end
    check("socket_budget", rejected ~= nil and
          string.find(tostring(rejected), "budget") ~= nil, rejected)
end

print(string.format("[net_loopback] done, failures=%d", failures))
