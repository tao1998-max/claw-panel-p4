---
{
  "name": "board_hardware_info",
  "description": "Use this skill before operating hardware or writing Lua and board-specific code that depends on device inventory and occupied GPIOs.",
  "metadata": {
    "cap_groups": []
  }
}
---

# Current Board Hardware: JC1060P470C_I_W_Y

Read this skill before operating hardware, assigning GPIOs, or writing Lua and
board-specific code. **You cannot speculate or fabricate hardware information.**

## Rules

- Before operating any hardware, read this skill first.
- Before assigning a GPIO, check whether it is already occupied below.
- When writing Lua or board-specific code, use the listed device names instead
  of guessing hardware wiring.

## Board Summary

- Board: `JC1060P470C_I_W_Y` (ESP32-P4 + ESP32-C6)
- Chip: `ESP32-P4` (RISC-V, 32 MB PSRAM, 16 MB flash)
- Version: board revision v1.3; firmware version is separate
- Display: 1024x600 MIPI-DSI, JD9165
- Touch: GT911 capacitive touch
- Audio: ES8311 codec (speaker + microphone)
- Wi-Fi: ESP32-C6 hosted over SDIO (ESP-Hosted 2.11.7, matching the host)
- RTC: EPSON RX8025T-UC at I2C 0x32 (stores UTC)
- Camera: OV02C10 hardware present, but **disabled by default** in this firmware
- BLE: C6 supports it, but host Bluetooth support is disabled in this build
- Not present/usable: IMU, IR, magnetometer, P1, board manager

## Device Inventory

### display

- Occupied IO:
  - `backlight` -> `GPIO23`
  - `reset` -> `GPIO0`

### touch

- Occupied IO:
  - `i2c.sda` -> `GPIO7`
  - `i2c.scl` -> `GPIO8`

### audio

- Occupied IO:
  - `i2s.sclk` -> `GPIO12`
  - `i2s.mclk` -> `GPIO13`
  - `i2s.lclk` -> `GPIO10`
  - `i2s.dout` -> `GPIO9`
  - `i2s.dsin` -> `GPIO48`
  - `power_amp` -> `GPIO11`
- Speech synthesis: Doubao TTS via `claw.speak(text[, timeout_ms])` from Lua
  (requires credentials in Claw → 设置 → 豆包语音).

### sdcard

- Occupied IO:
  - `sd.d0` -> `GPIO39`
  - `sd.d1` -> `GPIO40`
  - `sd.d2` -> `GPIO41`
  - `sd.d3` -> `GPIO42`
  - `sd.cmd` -> `GPIO44`
  - `sd.clk` -> `GPIO43`

### rs485

- Occupied IO:
  - `uart.tx` -> `GPIO26`
  - `uart.rx` -> `GPIO27`
- UART1, 115200 8N1, registered as `uart_bus:1` plus `gpio:26`/`gpio:27`.
  Lua owns the port through `require("uart").new(1, 26, 27, baud)`; the
  transceiver handles DE/RE, so there is no direction GPIO. No Modbus stack
  is installed: implement request/response framing in Lua.

### ethernet

- Occupied IO:
  - `smi.mdc` -> `GPIO31`
  - `smi.mdio` -> `GPIO52`
  - `phy.reset` -> `GPIO51`
- IP101 PHY, RMII; PHY clock input on GPIO50.

### wifi

- ESP32-C6 hosted over SDIO, running ESP-Hosted **2.11.7** (matches the host).
  Occupied by the host link:
  - `sdio.clk` -> `GPIO18`
  - `sdio.cmd` -> `GPIO19`
  - `sdio.d0..d3` -> `GPIO14`, `GPIO15`, `GPIO16`, `GPIO17`
  - `slave.reset` -> `GPIO54`
- C6 firmware updates are a board maintenance task (`POST /api/c6-ota` on the
  P4, image from `/sdcard/c6/network_adapter.bin`). Do not call it from an App.

### rtc

- EPSON RX8025T-UC at I2C address `0x32`, sharing the touch I2C bus
  (`SDA7` / `SCL8`).
- The chip stores **UTC**; the firmware seeds the system clock from it and
  syncs it back after SNTP. Do not write local time into it directly.

## Resource Registry

Only resources registered in the ESP-Claw hardware registry can be claimed:

- `uart_bus:1`, `gpio:26`, `gpio:27` (the RS485 port).

All other pins, even free header pins, are **not** claimable by Lua drivers in
this firmware. `gpio.new`, `i2c.new`, `adc.new`, `ledc.new`, `mcpwm.new`,
`pcnt.new` and `rmt.new` fail with a resource error for unregistered pins. Do
not offer to blink an LED or read a sensor on an unregistered pin; only the
RS485 port is available. `gpio:26`/`gpio:27` belong to UART1, so open them
through the `uart` module rather than as generic GPIO.

## Devices That Are NOT Present or Not Usable

Do not assume any of these are available to Apps:

- camera / MIPI-CSI sensor: OV02C10 hardware is present, but the firmware
  build has `CONFIG_TEMPLATE_ENABLE_CAMERA` off; do not use it unless the
  firmware is rebuilt with the camera enabled.
- Bluetooth / BLE: the C6 hardware supports it, but host Bluetooth support is
  disabled in this build.
- IMU, IR receiver/transmitter, magnetometer
- P1 connector / board manager
- onboard button, rotary knob, LED strip, smart-card (SCI) interface,
  environmental sensor, fuel gauge

If a user asks for one of these, say the hardware is either absent or disabled
in this firmware. Do not invent pins or call the corresponding Lua modules.

## Notes

- The GPIOs above are occupied by board peripherals; do not reuse them for
  generic GPIO/LED/PWM without checking the schematic.
- If a device has no explicit IO mapping here, treat it as unknown instead of
  guessing.
- For GPIO/I2C/ADC experiments, prefer free header pins and confirm them with
  the user before driving anything.
