/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * lv_animimg wrapper: plays a sequence of LVGL image sources.
 *
 * lv_animimg_set_src() stores the caller's pointer array instead of copying
 * it, so the array and every source string are kept in the object record and
 * released when the widget is deleted. Sources are LVGL FS paths, e.g.
 * "D:/apps/<id>/assets/frame0.png" (PNG/JPEG decoders are enabled).
 */

#include "lua_lvgl_private.h"

#define LUA_LVGL_ANIMIMG_DEFAULT_DURATION 1000
#define LUA_LVGL_ANIMIMG_MAX_SOURCES 1024

#if LV_USE_ANIMIMG

typedef struct {
    char **strings;
    const void **dsc;
    size_t count;
} lua_lvgl_animimg_src_t;

static lv_obj_t *lua_lvgl_animimg_validate_locked(const lua_lvgl_obj_ud_t *ud,
                                                  lua_lvgl_obj_type_t *out_type,
                                                  const char **out_error)
{
    lua_lvgl_obj_type_t type;
    lv_obj_t *obj = lua_lvgl_validate_ud_locked(ud, &type, out_error);

    if (!obj) {
        return NULL;
    }
    if (type != LUA_LVGL_OBJ_ANIMIMG) {
        if (out_error) {
            *out_error = "lvgl animimg method requires an animimg object";
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

static void lua_lvgl_animimg_src_free(lua_lvgl_animimg_src_t *src)
{
    size_t i;

    if (src->strings) {
        for (i = 0; i < src->count; i++) {
            free(src->strings[i]);
        }
        free(src->strings);
    }
    free(src->dsc);
    memset(src, 0, sizeof(*src));
}

/* Copy a Lua array of source strings. Non-throwing so it can run before the
 * LVGL lock is taken. On success the caller owns the allocated arrays. */
static bool lua_lvgl_animimg_src_parse(lua_State *L, int index, lua_lvgl_animimg_src_t *out,
                                       const char **out_error)
{
    size_t count;
    size_t i;
    char **strings;
    const void **dsc;

    memset(out, 0, sizeof(*out));
    if (!lua_istable(L, index)) {
        *out_error = "src must be a table of image paths";
        return false;
    }
    count = lua_rawlen(L, index);
    if (count == 0) {
        *out_error = "src must not be empty";
        return false;
    }
    if (count > LUA_LVGL_ANIMIMG_MAX_SOURCES) {
        *out_error = "src has too many entries";
        return false;
    }

    strings = calloc(count, sizeof(*strings));
    dsc = calloc(count, sizeof(*dsc));
    if (!strings || !dsc) {
        free(strings);
        free(dsc);
        *out_error = "src allocation failed";
        return false;
    }

    for (i = 1; i <= count; i++) {
        const char *value;
        size_t len;

        lua_rawgeti(L, index, (lua_Integer)i);
        if (lua_type(L, -1) != LUA_TSTRING) {
            lua_pop(L, 1);
            *out_error = "src entries must be strings";
            goto fail;
        }
        value = lua_tolstring(L, -1, &len);
        if (len == 0) {
            lua_pop(L, 1);
            *out_error = "src entries must not be empty";
            goto fail;
        }
        strings[i - 1] = malloc(len + 1);
        if (!strings[i - 1]) {
            lua_pop(L, 1);
            *out_error = "src allocation failed";
            goto fail;
        }
        memcpy(strings[i - 1], value, len + 1);
        dsc[i - 1] = strings[i - 1];
        lua_pop(L, 1);
    }

    out->strings = strings;
    out->dsc = dsc;
    out->count = count;
    return true;

fail:
    out->strings = strings;
    out->dsc = dsc;
    out->count = count;
    lua_lvgl_animimg_src_free(out);
    return false;
}

static void lua_lvgl_animimg_clear_src(lua_lvgl_obj_record_t *record)
{
    lua_lvgl_animimg_src_t src = {
        .strings = record->string_array,
        .dsc = (const void **)record->data,
        .count = record->string_array_count,
    };

    lua_lvgl_animimg_src_free(&src);
    record->string_array = NULL;
    record->string_array_count = 0;
    record->data = NULL;
    record->data_size = 0;
}

static void lua_lvgl_animimg_take_src(lua_lvgl_obj_record_t *record, lua_lvgl_animimg_src_t *src)
{
    record->string_array = src->strings;
    record->string_array_count = src->count;
    record->data = src->dsc;
    record->data_size = src->count;
    memset(src, 0, sizeof(*src));
}

#define LUA_LVGL_ANIMIMG_METHOD_BEGIN() \
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1); \
    esp_err_t err = lua_lvgl_lock(); \
    lv_obj_t *obj; \
    const char *obj_error = NULL; \
    if (err != ESP_OK) { \
        return lua_lvgl_error_esp(L, "lock", err); \
    } \
    obj = lua_lvgl_animimg_validate_locked(ud, NULL, &obj_error); \
    if (!obj) { \
        lua_lvgl_unlock(); \
        return luaL_error(L, "%s", obj_error); \
    }

static int lua_lvgl_animimg(lua_State *L)
{
    lua_lvgl_obj_ud_t *parent_ud = lua_lvgl_check_ud(L, 1);
    lua_lvgl_opts_t opts;
    lv_align_t align;
    esp_err_t err;
    lv_obj_t *parent;
    lv_obj_t *obj;
    lua_lvgl_obj_ud_t *created_ud;
    const char *obj_error = NULL;
    const char *src_error = NULL;
    lua_lvgl_animimg_src_t src = {0};
    bool has_src = false;
    bool autoplay = true;
    int duration = LUA_LVGL_ANIMIMG_DEFAULT_DURATION;
    int repeat_count = -1;
    uint32_t repeat;

    lua_lvgl_parse_opts(L, 2, &opts);
    if (opts.align_value && lua_lvgl_parse_align(L, opts.align_value, &align) != ESP_OK) {
        return luaL_error(L, "lvgl align must be top_left, top_mid, top_right, bottom_left, bottom_mid, bottom_right, left_mid, right_mid, or center");
    }

    if (opts.has_opts) {
        lua_getfield(L, 2, "src");
        if (!lua_isnil(L, -1)) {
            if (!lua_lvgl_animimg_src_parse(L, lua_gettop(L), &src, &src_error)) {
                lua_pop(L, 1);
                return luaL_error(L, "lvgl animimg %s", src_error);
            }
            has_src = true;
        }
        lua_pop(L, 1);

        if (lua_lvgl_has_field(L, 2, "duration")) {
            duration = lua_lvgl_get_opt_int_field(L, 2, "duration", LUA_LVGL_ANIMIMG_DEFAULT_DURATION);
        }
        if (lua_lvgl_has_field(L, 2, "repeat_count")) {
            repeat_count = lua_lvgl_get_opt_int_field(L, 2, "repeat_count", -1);
        }
        autoplay = lua_lvgl_get_opt_bool_field(L, 2, "autoplay", true);
    }
    if (duration <= 0) {
        lua_lvgl_animimg_src_free(&src);
        return luaL_error(L, "lvgl animimg duration must be positive");
    }
    repeat = repeat_count < 0 ? LV_ANIM_REPEAT_INFINITE : (uint32_t)repeat_count;

    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        lua_lvgl_animimg_src_free(&src);
        return lua_lvgl_error_esp(L, "lock", err);
    }
    parent = lua_lvgl_validate_ud_locked(parent_ud, NULL, &obj_error);
    if (!parent) {
        lua_lvgl_unlock();
        lua_lvgl_animimg_src_free(&src);
        return luaL_error(L, "%s", obj_error);
    }

    obj = lv_animimg_create(parent);
    if (!obj) {
        lua_lvgl_unlock();
        lua_lvgl_animimg_src_free(&src);
        return luaL_error(L, "lvgl animimg create failed");
    }
    if (opts.has_opts) {
        lua_lvgl_apply_common_opts_locked(obj, &opts);
        lua_lvgl_apply_style_opts_locked(L, 2, obj);
    }

    created_ud = lua_lvgl_push_obj(L, obj, LUA_LVGL_OBJ_ANIMIMG);
    if (!created_ud) {
        lv_obj_delete(obj);
        lua_lvgl_unlock();
        lua_lvgl_animimg_src_free(&src);
        return luaL_error(L, "lvgl object record allocation failed");
    }

    if (has_src) {
        lv_animimg_set_src(obj, src.dsc, src.count);
        lua_lvgl_animimg_take_src(created_ud->record, &src);
    }
    lv_animimg_set_duration(obj, (uint32_t)duration);
    lv_animimg_set_repeat_count(obj, repeat);
    if (autoplay && has_src) {
        lv_animimg_start(obj);
    }

    lua_lvgl_unlock();
    return 1;
}

int lua_lvgl_animimg_set_src(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    const char *src_error = NULL;
    lua_lvgl_animimg_src_t src = {0};
    esp_err_t err;
    lv_obj_t *obj;
    const char *obj_error = NULL;

    if (!lua_lvgl_animimg_src_parse(L, 2, &src, &src_error)) {
        return luaL_error(L, "lvgl animimg %s", src_error);
    }

    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        lua_lvgl_animimg_src_free(&src);
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_animimg_validate_locked(ud, NULL, &obj_error);
    if (!obj) {
        lua_lvgl_unlock();
        lua_lvgl_animimg_src_free(&src);
        return luaL_error(L, "%s", obj_error);
    }

    lv_animimg_set_src(obj, src.dsc, src.count);
    lua_lvgl_animimg_clear_src(ud->record);
    lua_lvgl_animimg_take_src(ud->record, &src);
    lv_animimg_start(obj);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_animimg_start(lua_State *L)
{
    bool started;

    LUA_LVGL_ANIMIMG_METHOD_BEGIN();
    started = ud->record->data != NULL;
    if (started) {
        lv_animimg_start(obj);
    }
    lua_lvgl_unlock();
    lua_pushboolean(L, started);
    return 1;
}

int lua_lvgl_animimg_stop(lua_State *L)
{
    LUA_LVGL_ANIMIMG_METHOD_BEGIN();
    lv_anim_delete(obj, NULL);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_animimg_set_duration(lua_State *L)
{
    int duration = (int)luaL_checkinteger(L, 2);

    luaL_argcheck(L, duration > 0, 2, "duration must be positive");
    LUA_LVGL_ANIMIMG_METHOD_BEGIN();
    lv_animimg_set_duration(obj, (uint32_t)duration);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_animimg_get_duration(lua_State *L)
{
    uint32_t duration;

    LUA_LVGL_ANIMIMG_METHOD_BEGIN();
    duration = lv_animimg_get_duration(obj);
    lua_lvgl_unlock();
    lua_pushinteger(L, (lua_Integer)duration);
    return 1;
}

int lua_lvgl_animimg_set_repeat_count(lua_State *L)
{
    int count = (int)luaL_checkinteger(L, 2);
    uint32_t repeat = count < 0 ? LV_ANIM_REPEAT_INFINITE : (uint32_t)count;

    LUA_LVGL_ANIMIMG_METHOD_BEGIN();
    lv_animimg_set_repeat_count(obj, repeat);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_animimg_get_repeat_count(lua_State *L)
{
    uint32_t repeat;

    LUA_LVGL_ANIMIMG_METHOD_BEGIN();
    repeat = lv_animimg_get_repeat_count(obj);
    lua_lvgl_unlock();
    lua_pushinteger(L, repeat == LV_ANIM_REPEAT_INFINITE ? -1 : (lua_Integer)repeat);
    return 1;
}

int lua_lvgl_animimg_get_src_count(lua_State *L)
{
    uint8_t count;

    LUA_LVGL_ANIMIMG_METHOD_BEGIN();
    count = lv_animimg_get_src_count(obj);
    lua_lvgl_unlock();
    lua_pushinteger(L, (lua_Integer)count);
    return 1;
}

const luaL_Reg lua_lvgl_animimg_module_funcs[] = {
    {"animimg", lua_lvgl_animimg},
    {NULL, NULL},
};

#else /* LV_USE_ANIMIMG */

static int lua_lvgl_animimg_disabled(lua_State *L)
{
    return luaL_error(L, "lvgl animimg is not enabled in firmware");
}

const luaL_Reg lua_lvgl_animimg_module_funcs[] = {
    {"animimg", lua_lvgl_animimg_disabled},
    {NULL, NULL},
};

#endif /* LV_USE_ANIMIMG */
