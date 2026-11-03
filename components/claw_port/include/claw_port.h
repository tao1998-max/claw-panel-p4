/*
 * ESP-Claw port layer for the P4 HMI template.
 *
 * Owns the startup order and the safety policy; the vendored ESP-Claw
 * components (claw_cap / cap_lua / cap_files / cap_scheduler ...) provide
 * the actual capability implementations.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Queue a product-specific Lua module for registration during
 *        claw_port_init().
 *
 * The Lua module registry is locked once the Claw capability groups start, so
 * callers must add extra modules before claw_port_init(). Product modules live
 * in main (e.g. audio) and are registered before the official module list is
 * sealed.
 */
esp_err_t claw_port_add_lua_module(const char *name, lua_CFunction open_fn);

/**
 * @brief Queue a board hardware resource for the ESP-Claw hardware registry.
 *
 * Official Lua hardware drivers (uart, gpio, i2c, ...) claim resources such as
 * "uart_bus:1" or "gpio:26" through claw_hw_registry. Only registered
 * resources can be claimed. Queue them before claw_port_init(); they are
 * registered when the registry is created.
 *
 * Build the key with claw_hw_key_uart_bus() / claw_hw_key_gpio() / etc.
 * Set shared=true for resources that several drivers may use concurrently;
 * false reserves an exclusive resource (one Lua object at a time).
 */
esp_err_t claw_port_add_hw_resource(const char *key, bool shared);

/**
 * @brief Start the Claw runtime.
 *
 * Called from app_start() before ai_start(). When CONFIG_TEMPLATE_ENABLE_CLAW
 * is disabled this is a no-op so the linker can drop the vendored code.
 *
 * @param sd_mount_point SD card mount point (e.g. "/sdcard"), or NULL. Two
 *        things are registered under it: "<mount>/apps" as a second, read-only
 *        app root, and the mount itself as an extra font root for
 *        lvgl.font_load(). Packages are launched in place from the card; the
 *        FAT workspace root stays first so it overrides duplicate ids and
 *        keeps publish/remove targeting FAT. A missing card is skipped, so the
 *        app list is simply empty when no card was inserted before boot.
 */
esp_err_t claw_port_init(const char *sd_mount_point);

/**
 * @brief True when the Claw runtime was initialized successfully.
 */
bool claw_port_is_ready(void);

/**
 * @brief Best-effort wipe of the Claw workspace (scripts, schedules, agent data).
 *
 * Called from the factory-reset path before NVS is erased. Never fails the
 * reset when Claw is disabled or not mounted.
 */
esp_err_t claw_port_factory_reset(void);

#ifdef __cplusplus
}
#endif
