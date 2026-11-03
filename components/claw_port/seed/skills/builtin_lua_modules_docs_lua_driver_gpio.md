# Lua GPIO

This module describes how to correctly use gpio when writing Lua scripts.

## How to call
- Import it with `local gpio = require("gpio")`
- Call `gpio.set_direction(pin, mode)` to set pin mode
- Call `gpio.set_level(pin, level)` to set output level
- Call `gpio.get_level(pin)` to read pin level
- Call `gpio.close(pin)` to reset and release the pin; repeated close is safe

Supported modes are `"input"`, `"output"`, `"input_output"`, `"output_od"`, `"input_output_od"`, and `"disable"`.

`set_direction` and `set_level` return no values on success and raise a Lua error on failure. `get_level` returns `0` or `1`.

A pin cannot be used while another device or script owns it. Call `gpio.close(pin)` to release it; otherwise it remains in use until the job exits. Garbage collection alone does not release pins. Retry a failed close before reusing the pin.

## Board resources (this panel)

Only pins registered in the ESP-Claw hardware registry can be claimed. This
firmware registers UART1's pins **GPIO26 / GPIO27** for the RS485 port; they
can be used as plain GPIO only while the UART is closed. Other pins raise
`resource not found` until the board registers them at boot.

The example uses GPIO26 because it is registered on this panel.

## Example
```lua
local gpio = require("gpio")
gpio.set_direction(26, "output")
gpio.set_level(26, 1)
gpio.close(26)
```
