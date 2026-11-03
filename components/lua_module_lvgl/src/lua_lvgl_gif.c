/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * lv_gif wrapper.
 *
 * lv_gif_set_src() keeps the source path pointer on the widget (used only
 * while opening the file and if the color format changes), so a copy of the
 * path is kept in the object record. Paths are LVGL FS paths, e.g.
 * "D:/apps/<id>/assets/anim.gif".
 */

#include "lua_lvgl_private.h"

#if LV_USE_GIF

static lv_obj_t *lua_lvgl_gif_validate_locked(const lua_lvgl_obj_ud_t *ud,
                                              lua_lvgl_obj_type_t *out_type,
                                              const char **out_error)
{
    lua_lvgl_obj_type_t type;
    lv_obj_t *obj = lua_lvgl_validate_ud_locked(ud, &type, out_error);

    if (!obj) {
        return NULL;
    }
    if (type != LUA_LVGL_OBJ_GIF) {
        if (out_error) {
            *out_error = "lvgl gif method requires a gif object";
        }
        if (out_type) {
            *out_type = type;
        }
        return NULL;
    }
    if (out_type) {
        *out_type = type;
    }
    return obj;
}

static bool lua_lvgl_gif_parse_color_format(const char *value, lv_color_format_t *out)
{
    if (!value || strcmp(value, "argb8888") == 0) {
        *out = LV_COLOR_FORMAT_ARGB8888;
    } else if (strcmp(value, "rgb565") == 0) {
        *out = LV_COLOR_FORMAT_RGB565;
    } else if (strcmp(value, "rgb888") == 0) {
        *out = LV_COLOR_FORMAT_RGB888;
    } else if (strcmp(value, "xrgb8888") == 0) {
        *out = LV_COLOR_FORMAT_XRGB8888;
    } else {
        return false;
    }
    return true;
}

static char *lua_lvgl_gif_copy_path(const char *path)
{
    size_t len = strlen(path);

    if (len == 0) {
        return NULL;
    }
    char *copy = malloc(len + 1);

    if (copy) {
        memcpy(copy, path, len + 1);
    }
    return copy;
}

#define LUA_LVGL_GIF_METHOD_BEGIN() \
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1); \
    esp_err_t err = lua_lvgl_lock(); \
    lv_obj_t *obj; \
    const char *obj_error = NULL; \
    if (err != ESP_OK) { \
        return lua_lvgl_error_esp(L, "lock", err); \
    } \
    obj = lua_lvgl_gif_validate_locked(ud, NULL, &obj_error); \
    if (!obj) { \
        lua_lvgl_unlock(); \
        return luaL_error(L, "%s", obj_error); \
    }

static int lua_lvgl_gif(lua_State *L)
{
    lua_lvgl_obj_ud_t *parent_ud = lua_lvgl_check_ud(L, 1);
    lua_lvgl_opts_t opts;
    lv_align_t align;
    lv_color_format_t color_format = LV_COLOR_FORMAT_ARGB8888;
    esp_err_t err;
    lv_obj_t *parent;
    lv_obj_t *obj;
    lua_lvgl_obj_ud_t *created_ud;
    const char *obj_error = NULL;
    const char *src = NULL;
    char *src_copy = NULL;
    bool has_color_format = false;
    bool has_loop_count = false;
    bool has_auto_pause = false;
    int loop_count = 0;
    bool auto_pause = false;

    lua_lvgl_parse_opts(L, 2, &opts);
    if (opts.align_value && lua_lvgl_parse_align(L, opts.align_value, &align) != ESP_OK) {
        return luaL_error(L, "lvgl align must be top_left, top_mid, top_right, bottom_left, bottom_mid, bottom_right, left_mid, right_mid, or center");
    }

    if (opts.has_opts) {
        const char *color = lua_lvgl_get_opt_string_field(L, 2, "color_format");

        if (color != NULL) {
            if (!lua_lvgl_gif_parse_color_format(color, &color_format)) {
                return luaL_error(L, "lvgl gif color_format must be argb8888, rgb565, rgb888, or xrgb8888");
            }
            has_color_format = true;
        }
        if (lua_lvgl_has_field(L, 2, "loop_count")) {
            loop_count = lua_lvgl_get_opt_int_field(L, 2, "loop_count", 0);
            has_loop_count = true;
        }
        if (lua_lvgl_has_field(L, 2, "auto_pause")) {
            auto_pause = lua_lvgl_get_opt_bool_field(L, 2, "auto_pause", false);
            has_auto_pause = true;
        }
        src = lua_lvgl_get_opt_string_field(L, 2, "src");
        if (src != NULL) {
            src_copy = lua_lvgl_gif_copy_path(src);
            if (!src_copy) {
                return luaL_error(L, "lvgl gif src must not be empty");
            }
        }
    }

    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        free(src_copy);
        return lua_lvgl_error_esp(L, "lock", err);
    }
    parent = lua_lvgl_validate_ud_locked(parent_ud, NULL, &obj_error);
    if (!parent) {
        lua_lvgl_unlock();
        free(src_copy);
        return luaL_error(L, "%s", obj_error);
    }

    obj = lv_gif_create(parent);
    if (!obj) {
        lua_lvgl_unlock();
        free(src_copy);
        return luaL_error(L, "lvgl gif create failed");
    }
    if (opts.has_opts) {
        lua_lvgl_apply_common_opts_locked(obj, &opts);
        lua_lvgl_apply_style_opts_locked(L, 2, obj);
    }
    if (has_color_format) {
        lv_gif_set_color_format(obj, color_format);
    }
    if (has_auto_pause) {
        lv_gif_set_auto_pause_invisible(obj, auto_pause);
    }

    created_ud = lua_lvgl_push_obj(L, obj, LUA_LVGL_OBJ_GIF);
    if (!created_ud) {
        lv_obj_delete(obj);
        lua_lvgl_unlock();
        free(src_copy);
        return luaL_error(L, "lvgl object record allocation failed");
    }

    if (src_copy) {
        lv_gif_set_src(obj, src_copy);
        created_ud->record->data = src_copy;
        created_ud->record->data_size = strlen(src_copy) + 1;
        src_copy = NULL;
        if (has_loop_count && lv_gif_is_loaded(obj)) {
            lv_gif_set_loop_count(obj, loop_count);
        }
    }

    lua_lvgl_unlock();
    return 1;
}

int lua_lvgl_gif_set_src(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    const char *src = luaL_checkstring(L, 2);
    char *copy = lua_lvgl_gif_copy_path(src);
    bool loaded;
    esp_err_t err;
    lv_obj_t *obj;
    const char *obj_error = NULL;

    if (!copy) {
        return luaL_error(L, "lvgl gif src must not be empty");
    }

    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        free(copy);
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_gif_validate_locked(ud, NULL, &obj_error);
    if (!obj) {
        lua_lvgl_unlock();
        free(copy);
        return luaL_error(L, "%s", obj_error);
    }

    lv_gif_set_src(obj, copy);
    free(ud->record->data);
    ud->record->data = copy;
    ud->record->data_size = strlen(copy) + 1;
    loaded = lv_gif_is_loaded(obj);
    lua_lvgl_unlock();
    lua_pushboolean(L, loaded);
    return 1;
}

int lua_lvgl_gif_restart(lua_State *L)
{
    LUA_LVGL_GIF_METHOD_BEGIN();
    lv_gif_restart(obj);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_gif_pause(lua_State *L)
{
    LUA_LVGL_GIF_METHOD_BEGIN();
    lv_gif_pause(obj);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_gif_resume(lua_State *L)
{
    LUA_LVGL_GIF_METHOD_BEGIN();
    lv_gif_resume(obj);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_gif_is_loaded(lua_State *L)
{
    bool loaded;

    LUA_LVGL_GIF_METHOD_BEGIN();
    loaded = lv_gif_is_loaded(obj);
    lua_lvgl_unlock();
    lua_pushboolean(L, loaded);
    return 1;
}

int lua_lvgl_gif_set_loop_count(lua_State *L)
{
    int32_t count = (int32_t)luaL_checkinteger(L, 2);
    bool loaded;

    LUA_LVGL_GIF_METHOD_BEGIN();
    loaded = lv_gif_is_loaded(obj);
    if (loaded) {
        lv_gif_set_loop_count(obj, count);
    }
    lua_lvgl_unlock();
    lua_pushboolean(L, loaded);
    return 1;
}

int lua_lvgl_gif_get_loop_count(lua_State *L)
{
    int32_t count = 0;

    LUA_LVGL_GIF_METHOD_BEGIN();
    if (lv_gif_is_loaded(obj)) {
        count = lv_gif_get_loop_count(obj);
    }
    lua_lvgl_unlock();
    lua_pushinteger(L, (lua_Integer)count);
    return 1;
}

int lua_lvgl_gif_get_frame_count(lua_State *L)
{
    int32_t count = 0;

    LUA_LVGL_GIF_METHOD_BEGIN();
    if (lv_gif_is_loaded(obj)) {
        count = lv_gif_get_frame_count(obj);
    }
    lua_lvgl_unlock();
    lua_pushinteger(L, (lua_Integer)count);
    return 1;
}

int lua_lvgl_gif_get_current_frame(lua_State *L)
{
    int32_t index = 0;

    LUA_LVGL_GIF_METHOD_BEGIN();
    if (lv_gif_is_loaded(obj)) {
        index = lv_gif_get_current_frame_index(obj);
    }
    lua_lvgl_unlock();
    lua_pushinteger(L, (lua_Integer)index);
    return 1;
}

int lua_lvgl_gif_set_auto_pause(lua_State *L)
{
    bool enabled = lua_toboolean(L, 2);

    luaL_checktype(L, 2, LUA_TBOOLEAN);
    LUA_LVGL_GIF_METHOD_BEGIN();
    lv_gif_set_auto_pause_invisible(obj, enabled);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_gif_get_size(lua_State *L)
{
    uint16_t w = 0;
    uint16_t h = 0;
    bool ok;

    LUA_LVGL_GIF_METHOD_BEGIN();
    if (ud->record->data == NULL) {
        lua_lvgl_unlock();
        lua_pushnil(L);
        return 1;
    }
    ok = lv_gif_get_size((const char *)ud->record->data, &w, &h);
    lua_lvgl_unlock();
    if (!ok) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, (lua_Integer)w);
    lua_pushinteger(L, (lua_Integer)h);
    return 2;
}

const luaL_Reg lua_lvgl_gif_module_funcs[] = {
    {"gif", lua_lvgl_gif},
    {NULL, NULL},
};

#else /* LV_USE_GIF */

static int lua_lvgl_gif_disabled(lua_State *L)
{
    return luaL_error(L, "lvgl gif is not enabled in firmware");
}

const luaL_Reg lua_lvgl_gif_module_funcs[] = {
    {"gif", lua_lvgl_gif_disabled},
    {NULL, NULL},
};

#endif /* LV_USE_GIF */
