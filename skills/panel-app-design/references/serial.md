# Serial / RS485 (UART)

The panel exposes an RS485 transceiver on UART1:

- **TX = GPIO26, RX = GPIO27, 115200 8N1**
- DE/RE direction is handled by the transceiver hardware; software sees a
  plain UART.

Use the official `uart` module:

```lua
local uart = require("uart")
local u = uart.new(1, 26, 27, 115200)

u:flush_input()
u:write({0x01, 0x03, 0x00, 0x00, 0x00, 0x01, 0x84, 0x0A})  -- Modbus RTU example
local reply = u:read(64, 500)   -- wait up to 500 ms
u:close()
```

## Rules

- The port is a **single-owner exclusive** resource. `close()` it (or let the
  App exit) before another script opens it. A second `uart.new(1, ...)` raises
  a Lua error while the first handle is open, so call it through `pcall` in UI
  code.
- `read(len[, timeout_ms])` and `read_line([max_len[, timeout_ms]])` return
  whatever arrived; `timeout_ms` is in milliseconds and `0` is non-blocking.
  The receive buffer is 1 KiB, so poll or use a timeout during long transfers.
- `write(data)` accepts a string or a table of byte values and blocks until the
  data is queued/sent.
- Request/response protocols (Modbus RTU, meters, RS485 sensors): call
  `flush_input()`, write the request, then read with a timeout; do not busy
  loop.
- UART0 is the console; never open port 0.
- UART1 is registered in the ESP-Claw hardware registry as `uart_bus:1` plus
  `gpio:26`/`gpio:27`; other Lua drivers cannot claim the same pins while the
  port is open.
- Do **not** use `require("gpio")` for a DE/RE direction pin: direction is
  automatic. General GPIO resources are not registered in this firmware; only
  GPIO26/27 are claimable, and only while the UART is closed.

## Inside an LVGL App

Open the port once, keep the handle in an upvalue, and close it when the App
exits. `uart.new` raises a Lua error when the port is busy or the pins are
claimed, so wrap it in `pcall` and show the reason in the UI instead of letting
the App die.

Do not block the UI loop with `read(len, 500)` during normal operation. Poll the
non-blocking form and drain what is available; the 1 KiB RX buffer gives about
80 ms of headroom at 115200 baud (11.5 KB/s), so a 20-40 ms poll is safe:

```lua
local uart = require("uart")
local delay = require("delay")

local u = nil
local ok, res = pcall(function() return uart.new(1, 26, 27, 115200) end)
if ok then
  u = res
else
  -- res is the error message; show "port busy" in the UI
end

while not claw.abort_requested() do
  if u then
    local chunk = u:read(512)        -- timeout_ms omitted: non-blocking
    if #chunk > 0 then
      -- append to the log / update the UI
    end
  end
  lvgl.process_events(20)
  delay.delay_ms(10)
end

if u then u:close() end
```

Use the blocking form (`read(len, 500)`) only inside a request/response step
where the UI is expected to pause and show a waiting state (an AT probe, a
Modbus transaction). If the App offers both a local serial mode and a TCP
bridge mode, close the current link before opening the other one and persist the
mode/baud in a small config file (`io.open`, like other Apps).

## Example: line-oriented device

```lua
local uart = require("uart")
local delay = require("delay")

local u = uart.new(1, 26, 27, 9600)
u:flush_input()
u:write("READ?\r\n")
delay.delay_ms(20)
local line = u:read_line(128, 500)   -- includes the trailing "\n"
print((line:gsub("[\r\n]+$", "")))
u:close()
```
