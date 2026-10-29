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
- Wi-Fi: ESP32-C6 hosted over SDIO
- Not present: camera, Bluetooth, IMU, IR, magnetometer, P1, board manager

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
- UART1, 115200 8N1. The default firmware only does loopback/echo; no Modbus
  stack is installed.

### ethernet

- Occupied IO:
  - `smi.mdc` -> `GPIO31`
  - `smi.mdio` -> `GPIO52`
  - `phy.reset` -> `GPIO51`
- IP101 PHY, RMII; PHY clock input on GPIO50.

### wifi

- ESP32-C6 hosted over SDIO. Occupied by the host link:
  - `sdio.clk` -> `GPIO18`
  - `sdio.cmd` -> `GPIO19`
  - `sdio.d0..d3` -> `GPIO14`, `GPIO15`, `GPIO16`, `GPIO17`
  - `slave.reset` -> `GPIO54`

## Devices That Are NOT Present

Do not assume any of these exist on this board:

- camera / MIPI-CSI sensor
- Bluetooth or BLE
- IMU, IR receiver/transmitter, magnetometer
- P1 connector / board manager
- onboard button, rotary knob, LED strip, smart-card (SCI) interface,
  environmental sensor, fuel gauge

If a user asks for one of these, say the board does not provide it. Do not
invent pins or call the corresponding Lua modules.

## Notes

- The GPIOs above are occupied by board peripherals; do not reuse them for
  generic GPIO/LED/PWM without checking the schematic.
- If a device has no explicit IO mapping here, treat it as unknown instead of
  guessing.
- For GPIO/I2C/ADC experiments, prefer free header pins and confirm them with
  the user before driving anything.
