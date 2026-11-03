/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int luaopen_lvgl(lua_State *L);
esp_err_t lua_module_lvgl_register(void);
esp_err_t lua_module_lvgl_register_with_data_root(const char *data_root);

/**
 * Register an SD card mount point as the LVGL file-system drive "S:".
 * Call before the Lua LVGL runtime is initialized (e.g. at boot). When the
 * card is absent, simply do not call this; the S: drive is then unavailable.
 */
esp_err_t lua_module_lvgl_set_sd_root(const char *sd_root);

/**
 * Allow lvgl.font_load() to read a TTF from an extra absolute VFS root, such
 * as the SD mount. Up to two roots are kept; paths under them are read
 * directly instead of being resolved against the Claw workspace. Call before
 * any Lua app runs (e.g. from app_start()).
 */
esp_err_t lua_lvgl_add_extra_font_root(const char *root);

#ifdef __cplusplus
}
#endif
