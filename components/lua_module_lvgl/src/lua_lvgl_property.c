/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Generic escape hatch over LVGL's LV_USE_OBJ_PROPERTY API.
 *
 *   obj:set_property("text", "hello")
 *   local text = obj:get_property("text")
 *
 * Property names are resolved with lv_obj_property_get_id(), which searches
 * the widget class hierarchy first and style properties second. Passing an
 * integer uses it as the raw lv_prop_id_t. Value conversion follows the type
 * bits embedded in the property ID, so no per-widget Lua code is needed.
 */

#include "lua_lvgl_private.h"

#if LV_USE_OBJ_PROPERTY

/* LV_PROPERTY_ID_TYPE2() in LVGL 9.5 shifts by the wrong constant; decode the
 * second value type directly from bits <27:24>. */
#define LUA_LVGL_PROP_TYPE1(id) ((uint32_t)(id) >> LV_PROPERTY_TYPE_SHIFT)
#define LUA_LVGL_PROP_TYPE2(id) (((uint32_t)(id) >> 24) & 0x0FU)

static bool lua_lvgl_prop_arg(lua_State *L, int index, const char **out_name, lv_prop_id_t *out_id)
{
    int type = lua_type(L, index);

    if (type == LUA_TSTRING) {
        *out_name = lua_tostring(L, index);
        *out_id = LV_PROPERTY_ID_INVALID;
        return true;
    }
    if ((type == LUA_TNUMBER) && lua_isinteger(L, index)) {
        *out_name = NULL;
        *out_id = (lv_prop_id_t)lua_tointeger(L, index);
        return true;
    }
    return false;
}

static lv_obj_t *lua_lvgl_prop_obj_locked(lua_State *L, int index, const char **out_error)
{
    lua_lvgl_obj_ud_t *ud = lua_touserdata(L, index);
    lua_lvgl_obj_type_t type;

    if (ud != NULL && lua_getmetatable(L, index)) {
        bool is_obj;

        lua_getfield(L, -1, "__lvgl_obj");
        is_obj = lua_toboolean(L, -1);
        lua_pop(L, 2);
        if (is_obj) {
            return lua_lvgl_validate_ud_locked(ud, &type, out_error);
        }
    }
    *out_error = "lvgl object expected";
    return NULL;
}

static lv_font_t *lua_lvgl_prop_font_locked(lua_State *L, int index, const char **out_error)
{
    lua_lvgl_font_ud_t *ud = (lua_lvgl_font_ud_t *)luaL_testudata(L, index, LUA_LVGL_FONT_MT);

    if (ud == NULL) {
        *out_error = "lvgl font expected";
        return NULL;
    }
    return lua_lvgl_validate_font_locked(ud, out_error);
}

static bool lua_lvgl_prop_point(lua_State *L, int index, lv_point_t *out)
{
    lua_Integer x = 0;
    lua_Integer y = 0;
    bool has_x = false;
    bool has_y = false;

    if (!lua_istable(L, index)) {
        return false;
    }

    lua_getfield(L, index, "x");
    if (lua_isinteger(L, -1)) {
        x = lua_tointeger(L, -1);
        has_x = true;
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "y");
    if (lua_isinteger(L, -1)) {
        y = lua_tointeger(L, -1);
        has_y = true;
    }
    lua_pop(L, 1);

    if (!has_x || !has_y) {
        has_x = false;
        has_y = false;
        lua_rawgeti(L, index, 1);
        if (lua_isinteger(L, -1)) {
            x = lua_tointeger(L, -1);
            has_x = true;
        }
        lua_pop(L, 1);
        lua_rawgeti(L, index, 2);
        if (lua_isinteger(L, -1)) {
            y = lua_tointeger(L, -1);
            has_y = true;
        }
        lua_pop(L, 1);
    }

    if (!has_x || !has_y) {
        return false;
    }
    out->x = (int32_t)x;
    out->y = (int32_t)y;
    return true;
}

/* Fill one union element of `prop` from the Lua value at `index`. `second`
 * selects the arg2 member for paired properties. Caller must hold the LVGL
 * lock for OBJ/FONT values. Never raises a Lua error. */
static bool lua_lvgl_prop_fill(lua_State *L, int index, uint32_t type,
                               lv_property_t *prop, bool second,
                               const char **out_error)
{
    switch (type) {
    case LV_PROPERTY_TYPE_INT:
        if (!lua_isinteger(L, index)) {
            *out_error = "integer expected";
            return false;
        }
        if (second) {
            prop->arg2.num = (int32_t)lua_tointeger(L, index);
        } else {
            prop->num = (int32_t)lua_tointeger(L, index);
        }
        return true;

    case LV_PROPERTY_TYPE_BOOL: {
        bool value;

        if (lua_isboolean(L, index)) {
            value = lua_toboolean(L, index);
        } else if (lua_isinteger(L, index)) {
            value = lua_tointeger(L, index) != 0;
        } else {
            *out_error = "boolean expected";
            return false;
        }
        if (second) {
            prop->arg2.enable = value;
        } else {
            prop->enable = value;
        }
        return true;
    }

    case LV_PROPERTY_TYPE_PRECISE: {
        lv_value_precise_t value;

        if (!lua_isnumber(L, index)) {
            *out_error = "number expected";
            return false;
        }
#if LV_USE_FLOAT
        value = (lv_value_precise_t)lua_tonumber(L, index);
#else
        if (!lua_isinteger(L, index)) {
            *out_error = "integer expected";
            return false;
        }
        value = (lv_value_precise_t)lua_tointeger(L, index);
#endif
        if (second) {
            prop->arg2.precise = value;
        } else {
            prop->precise = value;
        }
        return true;
    }

    case LV_PROPERTY_TYPE_COLOR: {
        lv_color_t color;

        if (lua_lvgl_parse_color(L, index, &color) != ESP_OK) {
            *out_error = "color must be a 0xRRGGBB number or '#RRGGBB' string";
            return false;
        }
        if (second) {
            prop->arg2.color = color;
        } else {
            prop->color = color;
        }
        return true;
    }

    case LV_PROPERTY_TYPE_POINT: {
        lv_point_t point;

        if (second) {
            *out_error = "point is not supported as a paired value";
            return false;
        }
        if (!lua_lvgl_prop_point(L, index, &point)) {
            *out_error = "point must be {x=..., y=...} or {x, y}";
            return false;
        }
        prop->point = point;
        return true;
    }

    case LV_PROPERTY_TYPE_TEXT:
    case LV_PROPERTY_TYPE_IMGSRC: {
        if (lua_type(L, index) != LUA_TSTRING) {
            *out_error = "string expected";
            return false;
        }
        if (second) {
            *out_error = "pointer value cannot be the second paired value";
            return false;
        }
        prop->ptr = lua_tostring(L, index);
        return true;
    }

    case LV_PROPERTY_TYPE_OBJ: {
        lv_obj_t *obj;

        if (second) {
            *out_error = "object value cannot be the second paired value";
            return false;
        }
        obj = lua_lvgl_prop_obj_locked(L, index, out_error);
        if (obj == NULL) {
            return false;
        }
        prop->ptr = obj;
        return true;
    }

    case LV_PROPERTY_TYPE_FONT: {
        lv_font_t *font;

        if (second) {
            *out_error = "font value cannot be the second paired value";
            return false;
        }
        font = lua_lvgl_prop_font_locked(L, index, out_error);
        if (font == NULL) {
            return false;
        }
        prop->ptr = font;
        return true;
    }

    case LV_PROPERTY_TYPE_POINTER:
        *out_error = "raw pointer values are not supported";
        return false;

    case LV_PROPERTY_TYPE_DISPLAY:
        *out_error = "display values are not supported";
        return false;

    default:
        *out_error = "property value type is not supported";
        return false;
    }
}

static bool lua_lvgl_prop_fill_value(lua_State *L, int index, uint32_t type1, uint32_t type2,
                                     lv_property_t *prop, const char **out_error)
{
    if (type2 == LV_PROPERTY_TYPE_INVALID) {
        return lua_lvgl_prop_fill(L, index, type1, prop, false, out_error);
    }

    /* LVGL 9.5's paired-value dispatch only handles these first types
     * reliably; pointer/text pairs would be silently ignored. */
    if (type1 != LV_PROPERTY_TYPE_INT && type1 != LV_PROPERTY_TYPE_BOOL) {
        *out_error = "paired properties are only supported with int/bool values";
        return false;
    }

    if (!lua_istable(L, index)) {
        /* A scalar sets the first value only; the paired value (usually an
         * animation flag) keeps its zero/false default. */
        return lua_lvgl_prop_fill(L, index, type1, prop, false, out_error);
    }

    lua_rawgeti(L, index, 1);
    bool ok = lua_lvgl_prop_fill(L, lua_gettop(L), type1, prop, false, out_error);
    lua_pop(L, 1);
    if (!ok) {
        return false;
    }

    lua_rawgeti(L, index, 2);
    ok = lua_lvgl_prop_fill(L, lua_gettop(L), type2, prop, true, out_error);
    lua_pop(L, 1);
    return ok;
}

int lua_lvgl_obj_set_property(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    const char *name = NULL;
    const char *obj_error = NULL;
    const char *value_error = NULL;
    lv_prop_id_t id = LV_PROPERTY_ID_INVALID;
    lv_property_t prop;
    uint32_t type1;
    uint32_t type2;
    lv_obj_t *obj;
    lv_result_t result;
    esp_err_t err;
    int selector;

    if (!lua_lvgl_prop_arg(L, 2, &name, &id)) {
        return luaL_error(L, "lvgl property name or id expected");
    }
    if (lua_isnoneornil(L, 3)) {
        return luaL_error(L, "lvgl property value expected");
    }
    selector = (int)luaL_optinteger(L, 4, 0);
    memset(&prop, 0, sizeof(prop));
    prop.selector = (uint32_t)selector;

    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }

    obj = lua_lvgl_validate_ud_locked(ud, NULL, &obj_error);
    if (obj == NULL) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }

    if (name != NULL) {
        id = lv_obj_property_get_id(obj, name);
        if (id == LV_PROPERTY_ID_INVALID) {
            lua_lvgl_unlock();
            return luaL_error(L, "unknown lvgl property '%s'", name);
        }
    }
    prop.id = id;

    type1 = LUA_LVGL_PROP_TYPE1(id);
    type2 = LUA_LVGL_PROP_TYPE2(id);
    if (type1 == LV_PROPERTY_TYPE_INVALID) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl property id 0x%08x has no value type; pass it by name",
                          (unsigned)id);
    }

    if (!lua_lvgl_prop_fill_value(L, 3, type1, type2, &prop, &value_error)) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl property value: %s", value_error);
    }

    result = lv_obj_set_property(obj, &prop);
    lua_lvgl_unlock();
    lua_pushboolean(L, result == LV_RESULT_OK);
    return 1;
}

int lua_lvgl_obj_get_property(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    const char *name = NULL;
    const char *obj_error = NULL;
    lv_prop_id_t id = LV_PROPERTY_ID_INVALID;
    lv_property_t value;
    uint32_t type1;
    lv_obj_t *obj;
    esp_err_t err;

    if (!lua_lvgl_prop_arg(L, 2, &name, &id)) {
        return luaL_error(L, "lvgl property name or id expected");
    }

    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }

    obj = lua_lvgl_validate_ud_locked(ud, NULL, &obj_error);
    if (obj == NULL) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }

    if (name != NULL) {
        id = lv_obj_property_get_id(obj, name);
        if (id == LV_PROPERTY_ID_INVALID) {
            lua_lvgl_unlock();
            return luaL_error(L, "unknown lvgl property '%s'", name);
        }
    }

    value = lv_obj_get_property(obj, id);
    if (value.id == LV_PROPERTY_ID_INVALID) {
        lua_lvgl_unlock();
        lua_pushnil(L);
        return 1;
    }

    type1 = LUA_LVGL_PROP_TYPE1(id);
    switch (type1) {
    case LV_PROPERTY_TYPE_INT:
        lua_pushinteger(L, value.num);
        break;

    case LV_PROPERTY_TYPE_BOOL:
        lua_pushboolean(L, value.enable);
        break;

    case LV_PROPERTY_TYPE_PRECISE:
        lua_pushnumber(L, (lua_Number)value.precise);
        break;

    case LV_PROPERTY_TYPE_COLOR:
        lua_pushinteger(L, (lua_Integer)(lv_color_to_u32(value.color) & 0xFFFFFFU));
        break;

    case LV_PROPERTY_TYPE_POINT:
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, value.point.x);
        lua_setfield(L, -2, "x");
        lua_pushinteger(L, value.point.y);
        lua_setfield(L, -2, "y");
        break;

    case LV_PROPERTY_TYPE_TEXT: {
        const char *text = (const char *)value.ptr;

        lua_pushstring(L, text != NULL ? text : "");
        break;
    }

    case LV_PROPERTY_TYPE_OBJ: {
        lv_obj_t *child = (lv_obj_t *)value.ptr;

        if (child == NULL || !lv_obj_is_valid(child)) {
            lua_pushnil(L);
        } else if (lua_lvgl_push_obj_ex(L, child, LUA_LVGL_OBJ_GENERIC, false) == NULL) {
            lua_pushnil(L);
        }
        break;
    }

    default:
        /* IMGSRC, POINTER, DISPLAY and FONT cannot be represented safely in
         * Lua yet; report "no readable value" rather than a dangling handle. */
        lua_pushnil(L);
        break;
    }

    lua_lvgl_unlock();
    return 1;
}

#else /* LV_USE_OBJ_PROPERTY */

int lua_lvgl_obj_set_property(lua_State *L)
{
    return luaL_error(L, "lvgl set_property is not enabled in firmware");
}

int lua_lvgl_obj_get_property(lua_State *L)
{
    return luaL_error(L, "lvgl get_property is not enabled in firmware");
}

#endif /* LV_USE_OBJ_PROPERTY */
