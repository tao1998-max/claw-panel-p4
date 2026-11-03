/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Lottie playback built on LVGL's lv_lottie widget (ThorVG software renderer).
 *
 * The widget renders into an ARGB8888 draw buffer sized `w x h`; the buffer is
 * allocated through lv_draw_buf_create() (LVGL's allocator, PSRAM-preferred in
 * this project) and released when the widget record is torn down.
 *
 * The generated animation is infinite by default. Finite repeat counts are
 * intentionally not exposed: LVGL frees a finished lv_anim_t and lv_lottie
 * keeps a dangling pointer, so a later set_src()/restart() would crash.
 */

#include "lua_lvgl_private.h"

#define LUA_LVGL_LOTTIE_DEFAULT_SIZE 200

#if LV_USE_LOTTIE

static lv_obj_t *lua_lvgl_lottie_validate_locked(const lua_lvgl_obj_ud_t *ud,
                                                 lua_lvgl_obj_type_t *out_type,
                                                 const char **out_error)
{
    lua_lvgl_obj_type_t type;
    lv_obj_t *obj = lua_lvgl_validate_ud_locked(ud, &type, out_error);

    if (!obj) {
        return NULL;
    }
    if (type != LUA_LVGL_OBJ_LOTTIE) {
        if (out_error) {
            *out_error = "lvgl lottie method requires a lottie object";
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

static lv_anim_t *lua_lvgl_lottie_anim(lv_obj_t *obj)
{
    return lv_lottie_get_anim(obj);
}

static bool lua_lvgl_lottie_loaded(lv_obj_t *obj)
{
    lv_anim_t *anim = lua_lvgl_lottie_anim(obj);

    /* lv_anim_get_playtime() reports LV_ANIM_PLAYTIME_INFINITE for the widget's
     * infinite animation, so use the single-cycle duration instead. */
    return anim != NULL && anim->duration > 0;
}

#define LUA_LVGL_LOTTIE_METHOD_BEGIN() \
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1); \
    esp_err_t err = lua_lvgl_lock(); \
    lv_obj_t *obj; \
    const char *obj_error = NULL; \
    if (err != ESP_OK) { \
        return lua_lvgl_error_esp(L, "lock", err); \
    } \
    obj = lua_lvgl_lottie_validate_locked(ud, NULL, &obj_error); \
    if (!obj) { \
        lua_lvgl_unlock(); \
        return luaL_error(L, "%s", obj_error); \
    }

static int lua_lvgl_lottie(lua_State *L)
{
    lua_lvgl_obj_ud_t *parent_ud = lua_lvgl_check_ud(L, 1);
    lua_lvgl_opts_t opts;
    lv_align_t align;
    esp_err_t err;
    lv_obj_t *parent;
    lv_obj_t *obj;
    lua_lvgl_obj_ud_t *created_ud;
    const char *obj_error = NULL;
    const char *src = NULL;
    const char *src_data = NULL;
    size_t src_data_len = 0;
    bool autoplay = true;
    int w;
    int h;
    lv_draw_buf_t *draw_buf;

    lua_lvgl_parse_opts(L, 2, &opts);
    if (opts.align_value && lua_lvgl_parse_align(L, opts.align_value, &align) != ESP_OK) {
        return luaL_error(L, "lvgl align must be top_left, top_mid, top_right, bottom_left, bottom_mid, bottom_right, left_mid, right_mid, or center");
    }
    w = opts.w > 0 ? opts.w : LUA_LVGL_LOTTIE_DEFAULT_SIZE;
    h = opts.h > 0 ? opts.h : LUA_LVGL_LOTTIE_DEFAULT_SIZE;

    if (opts.has_opts) {
        lua_getfield(L, 2, "src_data");
        if (!lua_isnil(L, -1)) {
            if (lua_type(L, -1) != LUA_TSTRING) {
                lua_pop(L, 1);
                return luaL_error(L, "lvgl lottie src_data must be a string");
            }
            src_data = lua_tolstring(L, -1, &src_data_len);
            if (src_data_len == 0) {
                lua_pop(L, 1);
                return luaL_error(L, "lvgl lottie src_data must not be empty");
            }
        }
        lua_pop(L, 1);

        if (src_data == NULL) {
            lua_getfield(L, 2, "src");
            if (!lua_isnil(L, -1)) {
                if (lua_type(L, -1) != LUA_TSTRING) {
                    lua_pop(L, 1);
                    return luaL_error(L, "lvgl lottie src must be a string");
                }
                src = lua_tostring(L, -1);
            }
            lua_pop(L, 1);
        }
        autoplay = lua_lvgl_get_opt_bool_field(L, 2, "autoplay", true);
    }

    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    parent = lua_lvgl_validate_ud_locked(parent_ud, NULL, &obj_error);
    if (!parent) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }

    obj = lv_lottie_create(parent);
    if (!obj) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl lottie create failed");
    }
    if (opts.has_opts) {
        lua_lvgl_apply_common_opts_locked(obj, &opts);
        lua_lvgl_apply_style_opts_locked(L, 2, obj);
    }

    draw_buf = lv_draw_buf_create((uint32_t)w, (uint32_t)h,
                                  LV_COLOR_FORMAT_ARGB8888_PREMULTIPLIED, 0);
    if (!draw_buf) {
        lv_obj_delete(obj);
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl lottie buffer allocation failed (%dx%d)", w, h);
    }
    lv_lottie_set_draw_buf(obj, draw_buf);
    lv_obj_set_size(obj, w, h);

    created_ud = lua_lvgl_push_obj(L, obj, LUA_LVGL_OBJ_LOTTIE);
    if (!created_ud) {
        lv_obj_delete(obj);
        lv_draw_buf_destroy(draw_buf);
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl object record allocation failed");
    }
    created_ud->record->draw_buf = draw_buf;

    if (src_data != NULL) {
        lv_lottie_set_src_data(obj, src_data, src_data_len);
    } else if (src != NULL) {
        lv_lottie_set_src_file(obj, src);
    }
    if (!autoplay) {
        lv_anim_t *anim = lua_lvgl_lottie_anim(obj);

        if (anim) {
            lv_anim_pause(anim);
        }
    }

    lua_lvgl_unlock();
    return 1;
}

int lua_lvgl_lottie_set_src(lua_State *L)
{
    const char *src = luaL_checkstring(L, 2);
    bool loaded;

    LUA_LVGL_LOTTIE_METHOD_BEGIN();

    lv_lottie_set_src_file(obj, src);
    loaded = lua_lvgl_lottie_loaded(obj);
    lua_lvgl_unlock();
    lua_pushboolean(L, loaded);
    return 1;
}

int lua_lvgl_lottie_set_src_data(lua_State *L)
{
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    bool loaded;

    LUA_LVGL_LOTTIE_METHOD_BEGIN();

    if (len == 0) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl lottie src_data must not be empty");
    }
    lv_lottie_set_src_data(obj, data, len);
    loaded = lua_lvgl_lottie_loaded(obj);
    lua_lvgl_unlock();
    lua_pushboolean(L, loaded);
    return 1;
}

int lua_lvgl_lottie_restart(lua_State *L)
{
    LUA_LVGL_LOTTIE_METHOD_BEGIN();
    lv_anim_t *anim = lua_lvgl_lottie_anim(obj);

    if (anim) {
        anim->act_time = 0;
        lv_anim_resume(anim);
    }
    lua_lvgl_unlock();
    lua_pushboolean(L, anim != NULL);
    return 1;
}

int lua_lvgl_lottie_pause(lua_State *L)
{
    LUA_LVGL_LOTTIE_METHOD_BEGIN();
    lv_anim_t *anim = lua_lvgl_lottie_anim(obj);

    if (anim) {
        lv_anim_pause(anim);
    }
    lua_lvgl_unlock();
    lua_pushboolean(L, anim != NULL);
    return 1;
}

int lua_lvgl_lottie_resume(lua_State *L)
{
    LUA_LVGL_LOTTIE_METHOD_BEGIN();
    lv_anim_t *anim = lua_lvgl_lottie_anim(obj);

    if (anim) {
        lv_anim_resume(anim);
    }
    lua_lvgl_unlock();
    lua_pushboolean(L, anim != NULL);
    return 1;
}

int lua_lvgl_lottie_is_loaded(lua_State *L)
{
    bool loaded;

    LUA_LVGL_LOTTIE_METHOD_BEGIN();
    loaded = lua_lvgl_lottie_loaded(obj);
    lua_lvgl_unlock();
    lua_pushboolean(L, loaded);
    return 1;
}

int lua_lvgl_lottie_is_playing(lua_State *L)
{
    bool playing;

    LUA_LVGL_LOTTIE_METHOD_BEGIN();
    lv_anim_t *anim = lua_lvgl_lottie_anim(obj);

    playing = anim != NULL && !anim->is_paused && anim->duration > 0;
    lua_lvgl_unlock();
    lua_pushboolean(L, playing);
    return 1;
}

int lua_lvgl_lottie_get_duration(lua_State *L)
{
    int32_t duration = 0;

    LUA_LVGL_LOTTIE_METHOD_BEGIN();
    lv_anim_t *anim = lua_lvgl_lottie_anim(obj);

    if (anim) {
        duration = anim->duration;
    }
    lua_lvgl_unlock();
    lua_pushinteger(L, (lua_Integer)duration);
    return 1;
}

int lua_lvgl_lottie_get_total_frames(lua_State *L)
{
    int32_t frames = 0;

    LUA_LVGL_LOTTIE_METHOD_BEGIN();
    lv_anim_t *anim = lua_lvgl_lottie_anim(obj);

    if (anim) {
        frames = anim->end_value;
    }
    lua_lvgl_unlock();
    lua_pushinteger(L, (lua_Integer)frames);
    return 1;
}

int lua_lvgl_lottie_get_time(lua_State *L)
{
    int32_t time = 0;

    LUA_LVGL_LOTTIE_METHOD_BEGIN();
    lv_anim_t *anim = lua_lvgl_lottie_anim(obj);

    if (anim && anim->act_time > 0) {
        time = anim->act_time;
    }
    lua_lvgl_unlock();
    lua_pushinteger(L, (lua_Integer)time);
    return 1;
}

const luaL_Reg lua_lvgl_lottie_module_funcs[] = {
    {"lottie", lua_lvgl_lottie},
    {NULL, NULL},
};

#else /* LV_USE_LOTTIE */

static int lua_lvgl_lottie_disabled(lua_State *L)
{
    return luaL_error(L, "lvgl lottie is not enabled in firmware");
}

const luaL_Reg lua_lvgl_lottie_module_funcs[] = {
    {"lottie", lua_lvgl_lottie_disabled},
    {NULL, NULL},
};

#endif /* LV_USE_LOTTIE */
