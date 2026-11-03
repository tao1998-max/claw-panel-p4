---
{
  "name": "builtin_lua_modules",
  "description": "Built-in Lua module documentation.",
  "metadata": {
    "cap_groups": [
      "cap_lua"
    ]
  }
}
---

# Builtin Lua Modules

To read documentation for a module, call `read_file("{CUR_SKILL_DIR}/scripts/docs/<Doc file path>")`.
Read all the files you need in one go as much as possible.
Do not fabricate functions that are not documented.

> **Hardware resource note:** hardware-driver modules (`gpio`, `adc`, `i2c`,
> `ledc`, `mcpwm`, `pcnt`, `rmt`) can only claim resources registered in the
> ESP-Claw hardware registry. This firmware registers UART1 for the RS485 port
> (`uart_bus:1`, GPIO26/27); other pins are not registered, so their `new()` or
> pin calls fail with a resource error. Use `uart.new(1, 26, 27, 115200)` for
> RS485 (direction EN is automatic).
>
> `button`, `knob`, `led_strip`, `sci` and `environmental_sensor` are not
> registered on this board; their docs are only seeded when
> `CONFIG_TEMPLATE_CLAW_LUA_UNUSED_HW_MODULES` is enabled for custom hardware.
> Do not try to `require` them on the stock firmware.

| Module | Doc file path |
| --- | --- |
| `lua_driver_adc` | `lua_driver_adc.md` |
| `lua_driver_gpio` | `lua_driver_gpio.md` |
| `lua_driver_i2c` | `lua_driver_i2c.md` |
| `lua_driver_ledc` | `lua_driver_ledc.md` |
| `lua_driver_mcpwm` | `lua_driver_mcpwm.md` |
| `lua_driver_pcnt` | `lua_driver_pcnt.md` |
| `lua_driver_rmt` | `lua_driver_rmt.md` |
| `lua_driver_uart` | `lua_driver_uart.md` |
| `lua_module_audio` | `lua_module_audio.md` |
| `lua_module_call_capability` | `lua_module_call_capability.md` |
| `lua_module_claw` | `lua_module_claw.md` |
| `lua_module_delay` | `lua_module_delay.md` |
| `lua_module_display` | `lua_module_display.md` |
| `lua_module_event_publisher` | `lua_module_event_publisher.md` |
| `lua_module_http_server` | `lua_module_http_server.md` |
| `lua_module_image` | `lua_module_image.md` |
| `lua_module_json` | `lua_module_json.md` |
| `lua_module_lvgl` | `lua_module_lvgl.md` |
| `lua_module_mqtt` | `lua_module_mqtt.md` |
| `lua_module_net` | `lua_module_net.md` |
| `lua_module_websocket` | `lua_module_websocket.md` |
| `lua_module_storage` | `lua_module_storage.md` |
| `lua_module_system` | `lua_module_system.md` |
| `lua_module_thread` | `lua_module_thread.md` |
