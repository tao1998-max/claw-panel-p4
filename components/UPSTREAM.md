# Vendored ESP-Claw components

Source: <https://github.com/espressif/esp-claw>
Commit: `9b23c15bc28703369b4b7fad3e810f8cb535fb59`
License: Apache-2.0 (see `LICENSE.esp-claw` in this directory and the SPDX
headers in every source file).

Copied from upstream `components/`:

| Local component | Upstream path |
| --- | --- |
| `claw_cap` | `claw_modules/claw_cap` |
| `claw_core` | `claw_modules/claw_core` |
| `claw_memory` | `claw_modules/claw_memory` |
| `claw_skill` | `claw_modules/claw_skill` |
| `claw_event_router` | `claw_modules/claw_event_router` |
| `claw_manager` | `claw_modules/claw_manager` |
| `claw_endpoint` | `claw_modules/claw_endpoint` |
| `claw_http` | `claw_modules/claw_http` |
| `claw_utils` | `claw_modules/claw_utils` |
| `claw_hw_registry` | `common/claw_hw_registry` |
| `cap_lua` | `claw_capabilities/cap_lua` |
| `cap_files` | `claw_capabilities/cap_files` |
| `cap_scheduler` | `claw_capabilities/cap_scheduler` |
| `cap_skill_mgr` | `claw_capabilities/cap_skill_mgr` |
| `cap_router_mgr` | `claw_capabilities/cap_router_mgr` |
| `cap_agent_mgr` | `claw_capabilities/cap_agent_mgr` |
| `cap_session_mgr` | `claw_capabilities/cap_session_mgr` |
| `cap_llm_config` | `claw_capabilities/cap_llm_config` |
| `cap_llm_inspect` | `claw_capabilities/cap_llm_inspect` |
| `cap_system` | `claw_capabilities/cap_system` |
| `cap_http_request` | `claw_capabilities/cap_http_request` |
| `cap_web_search` | `claw_capabilities/cap_web_search` |
| `cap_im_local` | `claw_capabilities/cap_im_local` |
| `lua_module_call_capability` | `lua_modules/lua_module_call_capability` |
| `lua_module_json` | `lua_modules/lua_module_json` |
| `lua_module_system` | `lua_modules/lua_module_system` |
| `lua_module_delay` | `lua_modules/lua_module_delay` |
| `lua_module_thread` | `lua_modules/lua_module_thread` |
| `lua_module_storage` | `lua_modules/lua_module_storage` |
| `lua_module_event_publisher` | `lua_modules/lua_module_event_publisher` |
| `lua_module_http_server` | `lua_modules/lua_module_http_server` |

Official skill assets copied verbatim into `claw_port/seed/` and written to
`/fatfs/claw/system/` on first boot:

| Seed file | Upstream path |
| --- | --- |
| `scheduled_task_SKILL.md` | `application/edge_agent/fatfs_image/storage/skills/scheduled_task/SKILL.md` |
| `scheduled_task.lua` | `application/edge_agent/fatfs_image/storage/skills/scheduled_task/scripts/add_scheduled_task.lua` |
| `cap_router_mgr_SKILL.md` | `claw_capabilities/cap_router_mgr/skills/cap_router_mgr/SKILL.md` |
| `cap_scheduler_SKILL.md` | `claw_capabilities/cap_scheduler/skills/cap_scheduler/SKILL.md` |
| `arg_schema.lua` | `lua_modules/lua_module_system/lib/arg_schema.lua` |
| `memory_ops_SKILL.md` | `claw_modules/claw_memory/skills/memory_ops/SKILL.md` |
| `profile_memory_ops_SKILL.md` | `claw_modules/claw_memory/skills/profile_memory_ops/SKILL.md` |
| `cap_http_request_SKILL.md` | `claw_capabilities/cap_http_request/skills/cap_http_request/SKILL.md` |
| `cap_web_search_SKILL.md` | `claw_capabilities/cap_web_search/skills/cap_web_search/SKILL.md` |
| `cap_llm_inspect_image_SKILL.md` | `claw_capabilities/cap_llm_inspect/skills/cap_llm_inspect_image/SKILL.md` |

Components and skills added after the first drop (now present under
`components/`):

- Capabilities: `cap_app_mgr`, `cap_boards` (generator only; no C sources),
  `cap_cli` (group registered, no CLI commands), `cap_im_platform` (QQ /
  WeChat / Feishu / Telegram providers).
- Services: `display_service`, `esp_painter`, `lua_module_builder`,
  `skill_builder`.
- Lua modules: all `lua_driver_*`, `lua_module_display`, `lua_module_lvgl`,
  `lua_module_image`, `lua_module_button`, `lua_module_knob`,
  `lua_module_led_strip`, `lua_module_sci`,
  `lua_module_environmental_sensor`. The product audio binding lives in
  `main/audio/lua_module_audio.c`. `lua_module_camera` is source-only and not
  built; `lua_module_fuel_gauge` is an empty CMake shell.
- Official skills: `light_switch`, `plan_mode`, `app_creator`,
  `skills_lab_search/downloader`, `skill_creator`, `lua_package_authoring`,
  plus a static `board_hardware_info` skill for this board.
- Official Web UI: `components/http_server` (prebuilt frontend + APIs) with
  product wiring in `main/web` and `components/app_config`.

Still absent or replaced:

- `application/edge_agent`, `common/app_claw` -> product `claw_port` and
  `claw_agent_mgr`/`claw_core` wiring.
- `audio_hub`, `audio_playback` -> XiaoZhi `AudioService` +
  `main/audio/lua_module_audio.c`.
- `system_ui` -> `main/ui`; `wifi_manager` -> `main/hal/hw_wifi.c`;
  `settings` -> `main/core/settings.c` + `components/app_config`.
- `captive_dns` -> not ported; the device is STA-only, no AP/captive portal.
- `cap_mcp_client`, `cap_mcp_server`, `mcp_mdns` -> not ported; the product
  exposes XiaoZhi MCP tools (`self.*`) and the `self.claw.*` bridge instead.
- `http_reuse` -> deliberately dropped (linker wraps would affect XiaoZhi's
  HTTPS client).
- BLE, camera/vision, board manager, touch/IMU/IR/magnetometer modules ->
  excluded by product decision. Touch/IR helpers are seeded as pure Lua
  libraries (`lib_si12t_touch.lua`, `ir_driver.lua`).
- `lua_module_button`, `lua_module_knob`, `lua_module_led_strip`,
  `lua_module_sci`, `lua_module_environmental_sensor` are compiled but only
  registered when `CONFIG_TEMPLATE_CLAW_LUA_UNUSED_HW_MODULES=y`; this board
  has none of those devices, so the default is `n`.
- CLI command sources (`cmd_*`) are not built. The official LVGL Claw page is
  not a port of the Web UI HTML/JS; it follows the same information
  architecture.

Local patches versus upstream:

1. All `idf_component.yml` files removed except `cap_lua/idf_component.yml`,
   which keeps only the `espressif/lua ^5.5.1` dependency. Upstream cross
   component `path:` dependencies are replaced by CMake `REQUIRES`.
2. `espressif__cjson` replaced with the IDF built-in `json` component
   (`claw_skill`, plus explicit `json` in components that include `cJSON.h`).
3. `claw_core` no longer requires `http_reuse` (unused; its global
   `esp_http_client_*` linker wraps would affect the XiaoZhi HTTPS client).
4. Test apps, host tests and `cmd_*` CLI sources are not built
   (`CONFIG_APP_CLAW_ENABLE_CLI` is not defined in this project).
5. `claw_core` and `cap_scheduler` task stacks use
   `CLAW_TASK_STACK_INTERNAL_ONLY`: both can write NVS/FAT, which disables the
   flash cache; a PSRAM stack would fault on ESP32-P4.
6. `cap_router_mgr`'s CLI source/header are omitted; the capability itself is
   unchanged. `cap_http_request` drops the unused `http_reuse` REQUIRES; its
   source is unchanged and it calls the standard `esp_http_client` API.
7. `lua_module_storage` adds `-Wno-error=format-truncation` for one upstream
   `snprintf` warning; the source is unchanged.
8. `cap_lua` keeps upstream's `luaL_openlibs()` and sandbox hooks, but creates
   the state with a PSRAM-first `lua_Alloc` instead of `luaL_newstate()`
   (`CONFIG_LUA_USE_PSRAM` only moves allocations >= 4 KiB; the per-widget Lua
   objects in a UI app still exhausted internal RAM). Internal RAM is shared
   with TLS/AES, ESP-Hosted SDIO and audio on this board; without this, opening
   a Lua app while XiaoZhi's WebSocket is connected asserts in
   `sdio_push_data_to_queue()`. `arg_schema.lua` is seeded to
   `system/scripts/builtin/lib/` and loaded through
   `cap_lua_add_package_path_dir()`, matching the upstream edge_agent layout.
9. The product Agent now runs on `claw_agent_mgr`: `claw_agent.c` calls
   `claw_agent_mgr_init()` / `create_root_agent()` and routes requests through
   `post_root_message` + `receive_root_for`. `cap_agent_mgr` and
   `cap_session_mgr` are registered; `cap_agent_mgr` is LLM-visible so the root
   Agent can spawn subagents. Session deletion is wired to
   `claw_memory_delete_session_history` + `claw_skill_delete_session_state`.
   Mirroring upstream `app_claw_start()`, `claw_port_init()` brings the
   manager/root core up at boot when the product Agent switch is on and the
   endpoint is configured, so the official Web UI's session/history APIs work
   before the first panel request.
10. `http_server` is based on upstream but is not source-identical: it carries
    product wiring such as the `/memory` file alias, the product schedules
    API, and the WeChat QR-login service callbacks supplied by `main/web`.
    WebIM session handlers remain upstream-compatible.
11. Seed writes trim the NUL terminator appended by IDF's `EMBED_TXTFILES`.
    Upstream skill scripts copied this way otherwise reach FAT with a trailing
    `\0`, which Lua reports as `unexpected symbol` at EOF.
12. The Agent keeps upstream's tool-iteration default (`64`), exposes
    `cap_app_manage`, and seeds the generated `builtin_lua_modules` skill
    (module README docs) so the official `app_creator` flow can run. The docs
    are generated from the vendored module READMEs, not hand-written.
13. The official Web UI (`components/http_server`) is wired to the product in
    `main/web/web_ui.c` + `components/app_config`: config load/save, IM
    credentials, timezone, WeChat QR login, schedules, files and WebIM.
14. Device-side dynamic Chinese uses the XiaoZhi `puhui` cbin font
    (`/fatfs/ai/font_puhui_common_20_4.bin`) through `ai_ui_text_font()`;
    the LVGL subset fonts remain for static i18n text.
15. `cap_boards` is an empty generator shell because the board manager is
    excluded. A static `board_hardware_info` skill is seeded instead so the
    official `light_switch` / `plan_mode` / `skills_lab` skills have a real
    device inventory to consult.
16. The board has no button/knob/LED strip/SCI/environmental sensor. Those
    Lua modules are opt-in via
    `CONFIG_TEMPLATE_CLAW_LUA_UNUSED_HW_MODULES` (default `n`); their seeded
    docs are removed when the option is off.

`Lua` itself is the managed `espressif/lua` component (pinned `^5.5.1`).
