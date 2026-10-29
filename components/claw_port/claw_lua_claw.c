/*
 * `claw` Lua module: the only application API visible to sandboxed scripts.
 *
 * Everything here runs on a Lua job task (never LVGL). File access is limited
 * to claw_paths roots; writing is limited to the DATA root.
 */
#include "claw_lua_claw.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lauxlib.h"

#include "cap_lua.h"
#include "claw_panel.h"
#include "claw_paths.h"
#include "claw_tasks.h"

static const char *TAG = "claw_lua";

#define CLAW_FILE_READ_MAX  (64 * 1024)
#define CLAW_FILE_WRITE_MAX (16 * 1024)
#define CLAW_PATH_MAX_REL   160
#define CLAW_JSON_MAX_DEPTH 16
#define CLAW_SLEEP_CHUNK_MS 100
#define CLAW_SLEEP_MAX_MS   5000

/* ---------- helpers ---------- */

static bool rel_path_ok(const char *path)
{
    if (!path || !path[0]) {
        return false;
    }
    size_t len = strlen(path);
    if (len >= CLAW_PATH_MAX_REL) {
        return false;
    }
    if (path[0] == '/') {
        return false;
    }
    if (strstr(path, "..")) {
        return false;
    }
    if (strchr(path, '\\')) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)path[i];
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
    }
    return true;
}

static bool build_path(const char *rel, bool writable, char *out, size_t out_size)
{
    const char *root = claw_paths_get(writable ? CLAW_PATH_DATA : CLAW_PATH_SYSTEM);
    if (!root || !root[0]) {
        return false;
    }
    int written = snprintf(out, out_size, "%s/%s", root, rel);
    return written > 0 && (size_t)written < out_size;
}

/* Read/list/exists may use DATA first, then SYSTEM. */
static bool build_read_path(const char *rel, char *out, size_t out_size)
{
    if (build_path(rel, true, out, out_size)) {
        struct stat st;
        if (stat(out, &st) == 0) {
            return true;
        }
    }
    return build_path(rel, false, out, out_size);
}

static int push_esp_result(lua_State *L, esp_err_t err)
{
    if (err == ESP_OK) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, esp_err_to_name(err));
    return 2;
}

static int push_fail(lua_State *L, const char *message)
{
    lua_pushnil(L);
    lua_pushstring(L, message);
    return 2;
}

/* ---------- basic functions ---------- */

static int l_log(lua_State *L)
{
    int n = lua_gettop(L);
    lua_getglobal(L, "print");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    lua_insert(L, 1);
    lua_call(L, n, 0);
    return 0;
}

static int l_time(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)time(NULL));
    return 1;
}

static int l_uptime_ms(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)(esp_timer_get_time() / 1000));
    return 1;
}

static int l_sleep(lua_State *L)
{
    lua_Integer ms = luaL_checkinteger(L, 1);
    if (ms < 0) {
        ms = 0;
    }
    if (ms > CLAW_SLEEP_MAX_MS) {
        ms = CLAW_SLEEP_MAX_MS;
    }

    while (ms > 0) {
        if (cap_lua_runtime_stop_requested(L)) {
            return luaL_error(L, "stopped by user");
        }
        lua_Integer step = ms > CLAW_SLEEP_CHUNK_MS ? CLAW_SLEEP_CHUNK_MS : ms;
        vTaskDelay(pdMS_TO_TICKS((uint32_t)step));
        ms -= step;
    }
    return 0;
}

static int l_progress(lua_State *L)
{
    lua_Integer percent = luaL_checkinteger(L, 1);
    const char *message = luaL_optstring(L, 2, "");
    const char *job_id = cap_lua_runtime_job_id(L);
    claw_task_report_progress(job_id, (int)percent, message);
    return 0;
}

static int l_abort_requested(lua_State *L)
{
    lua_pushboolean(L, cap_lua_runtime_stop_requested(L));
    return 1;
}

static int l_get_args(lua_State *L)
{
    lua_getglobal(L, "args");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    return 1;
}

/* ---------- file API ---------- */

static int l_file_read(lua_State *L)
{
    const char *rel = luaL_checkstring(L, 1);
    if (!rel_path_ok(rel)) {
        return push_fail(L, "invalid path");
    }
    char path[192];
    if (!build_read_path(rel, path, sizeof(path))) {
        return push_fail(L, "path not found");
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        return push_fail(L, "file not found");
    }
    if (st.st_size < 0 || st.st_size > CLAW_FILE_READ_MAX) {
        return push_fail(L, "file too large");
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        return push_fail(L, "open failed");
    }
    /* Scripts may read up to 64 KB; keep that copy out of internal RAM. */
    char *buf = heap_caps_malloc((size_t)st.st_size + 1,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc((size_t)st.st_size + 1);
    }
    if (!buf) {
        fclose(f);
        return push_fail(L, "out of memory");
    }
    size_t got = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    buf[got] = '\0';
    lua_pushlstring(L, buf, got);
    free(buf);
    return 1;
}

static int l_file_write(lua_State *L)
{
    const char *rel = luaL_checkstring(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    if (!rel_path_ok(rel)) {
        return push_fail(L, "invalid path");
    }
    if (len > CLAW_FILE_WRITE_MAX) {
        return push_fail(L, "content too large");
    }
    char path[192];
    if (!build_path(rel, true, path, sizeof(path))) {
        return push_fail(L, "invalid root");
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        return push_fail(L, "open failed");
    }
    size_t written = fwrite(data, 1, len, f);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    if (written != len) {
        return push_fail(L, "short write");
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_file_exists(lua_State *L)
{
    const char *rel = luaL_checkstring(L, 1);
    if (!rel_path_ok(rel)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    char path[192];
    if (!build_read_path(rel, path, sizeof(path))) {
        lua_pushboolean(L, 0);
        return 1;
    }
    struct stat st;
    lua_pushboolean(L, stat(path, &st) == 0);
    return 1;
}

static int l_file_list(lua_State *L)
{
    const char *rel = luaL_optstring(L, 1, ".");
    char path[192];

    if (strcmp(rel, ".") == 0 || rel[0] == '\0') {
        const char *root = claw_paths_get(CLAW_PATH_DATA);
        if (!root) {
            return push_fail(L, "no workspace");
        }
        strlcpy(path, root, sizeof(path));
    } else {
        if (!rel_path_ok(rel)) {
            return push_fail(L, "invalid path");
        }
        if (!build_read_path(rel, path, sizeof(path))) {
            return push_fail(L, "path not found");
        }
    }

    DIR *dir = opendir(path);
    if (!dir) {
        return push_fail(L, "open dir failed");
    }
    lua_newtable(L);
    int index = 1;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        lua_pushstring(L, entry->d_name);
        lua_rawseti(L, -2, index++);
    }
    closedir(dir);
    return 1;
}

static int l_file_remove(lua_State *L)
{
    const char *rel = luaL_checkstring(L, 1);
    if (!rel_path_ok(rel)) {
        return push_fail(L, "invalid path");
    }
    char path[192];
    if (!build_path(rel, true, path, sizeof(path))) {
        return push_fail(L, "invalid root");
    }
    if (unlink(path) != 0) {
        return push_fail(L, "remove failed");
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* ---------- JSON ---------- */

static cJSON *lua_to_cjson(lua_State *L, int index, int depth);

static cJSON *table_to_cjson(lua_State *L, int index, int depth)
{
    if (depth > CLAW_JSON_MAX_DEPTH) {
        return NULL;
    }
    index = lua_absindex(L, index);

    size_t array_len = lua_rawlen(L, index);
    if (array_len > 0) {
        cJSON *arr = cJSON_CreateArray();
        if (!arr) {
            return NULL;
        }
        for (size_t i = 1; i <= array_len; i++) {
            lua_rawgeti(L, index, (lua_Integer)i);
            cJSON *item = lua_to_cjson(L, -1, depth + 1);
            lua_pop(L, 1);
            if (!item) {
                cJSON_Delete(arr);
                return NULL;
            }
            cJSON_AddItemToArray(arr, item);
        }
        return arr;
    }

    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return NULL;
    }
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        const char *key = lua_tostring(L, -2);
        if (!key) {
            lua_pop(L, 2);
            cJSON_Delete(obj);
            return NULL;
        }
        cJSON *item = lua_to_cjson(L, -1, depth + 1);
        lua_pop(L, 1);
        if (!item) {
            lua_pop(L, 1);
            cJSON_Delete(obj);
            return NULL;
        }
        cJSON_AddItemToObject(obj, key, item);
    }
    return obj;
}

static cJSON *lua_to_cjson(lua_State *L, int index, int depth)
{
    if (depth > CLAW_JSON_MAX_DEPTH) {
        return NULL;
    }
    index = lua_absindex(L, index);
    switch (lua_type(L, index)) {
    case LUA_TNIL:
        return cJSON_CreateNull();
    case LUA_TBOOLEAN:
        return cJSON_CreateBool(lua_toboolean(L, index));
    case LUA_TNUMBER: {
        lua_Number n = lua_tonumber(L, index);
        if (n == (lua_Number)(lua_Integer)n) {
            return cJSON_CreateNumber((double)(lua_Integer)n);
        }
        return cJSON_CreateNumber((double)n);
    }
    case LUA_TSTRING:
        return cJSON_CreateString(lua_tostring(L, index));
    case LUA_TTABLE:
        return table_to_cjson(L, index, depth);
    default:
        return NULL;
    }
}

static void cjson_to_lua(lua_State *L, const cJSON *item, int depth)
{
    if (depth > CLAW_JSON_MAX_DEPTH) {
        lua_pushnil(L);
        return;
    }
    if (!item || cJSON_IsNull(item)) {
        lua_pushnil(L);
    } else if (cJSON_IsBool(item)) {
        lua_pushboolean(L, cJSON_IsTrue(item));
    } else if (cJSON_IsNumber(item)) {
        lua_pushnumber(L, item->valuedouble);
    } else if (cJSON_IsString(item)) {
        lua_pushstring(L, item->valuestring ? item->valuestring : "");
    } else if (cJSON_IsArray(item)) {
        lua_newtable(L);
        int i = 1;
        const cJSON *child = NULL;
        cJSON_ArrayForEach(child, item) {
            cjson_to_lua(L, child, depth + 1);
            lua_rawseti(L, -2, i++);
        }
    } else if (cJSON_IsObject(item)) {
        lua_newtable(L);
        const cJSON *child = NULL;
        cJSON_ArrayForEach(child, item) {
            cjson_to_lua(L, child, depth + 1);
            lua_setfield(L, -2, child->string ? child->string : "");
        }
    } else {
        lua_pushnil(L);
    }
}

static int l_json_encode(lua_State *L)
{
    cJSON *root = lua_to_cjson(L, 1, 0);
    if (!root) {
        return push_fail(L, "cannot encode value");
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        return push_fail(L, "encode failed");
    }
    lua_pushstring(L, text);
    cJSON_free(text);
    return 1;
}

static int l_json_decode(lua_State *L)
{
    size_t len = 0;
    const char *text = luaL_checklstring(L, 1, &len);
    cJSON *root = cJSON_ParseWithLength(text, len);
    if (!root) {
        return push_fail(L, "invalid json");
    }
    cjson_to_lua(L, root, 0);
    cJSON_Delete(root);
    return 1;
}

/* ---------- panel ---------- */

static int l_panel_set_volume(lua_State *L)
{
    int percent = (int)luaL_checkinteger(L, 1);
    return push_esp_result(L, claw_panel_set_volume(percent));
}

static int l_panel_set_brightness(lua_State *L)
{
    int percent = (int)luaL_checkinteger(L, 1);
    return push_esp_result(L, claw_panel_set_brightness(percent));
}

static int l_panel_status(lua_State *L)
{
    char buf[256] = {0};
    esp_err_t err = claw_panel_status(buf, sizeof(buf));
    if (err != ESP_OK) {
        return push_esp_result(L, err);
    }
    lua_pushstring(L, buf);
    return 1;
}

static int l_panel_show_message(lua_State *L)
{
    const char *text = luaL_checkstring(L, 1);
    return push_esp_result(L, claw_panel_show_message(text));
}

static int l_panel_demo(lua_State *L)
{
    bool on = lua_toboolean(L, 1) != 0;
    return push_esp_result(L, claw_panel_demo(on));
}

/* ---------- module ---------- */

static const luaL_Reg claw_funcs[] = {
    {"log", l_log},
    {"time", l_time},
    {"uptime_ms", l_uptime_ms},
    {"sleep", l_sleep},
    {"progress", l_progress},
    {"abort_requested", l_abort_requested},
    {"get_args", l_get_args},
    {NULL, NULL},
};

static const luaL_Reg file_funcs[] = {
    {"read", l_file_read},
    {"write", l_file_write},
    {"exists", l_file_exists},
    {"list", l_file_list},
    {"remove", l_file_remove},
    {NULL, NULL},
};

static const luaL_Reg json_funcs[] = {
    {"encode", l_json_encode},
    {"decode", l_json_decode},
    {NULL, NULL},
};

static const luaL_Reg panel_funcs[] = {
    {"set_volume", l_panel_set_volume},
    {"set_brightness", l_panel_set_brightness},
    {"status", l_panel_status},
    {"show_message", l_panel_show_message},
    {"demo", l_panel_demo},
    {NULL, NULL},
};

int luaopen_claw(lua_State *L)
{
    luaL_newlib(L, claw_funcs);

    lua_newtable(L);
    luaL_setfuncs(L, file_funcs, 0);
    lua_setfield(L, -2, "file");

    lua_newtable(L);
    luaL_setfuncs(L, json_funcs, 0);
    lua_setfield(L, -2, "json");

    lua_newtable(L);
    luaL_setfuncs(L, panel_funcs, 0);
    lua_setfield(L, -2, "panel");


    ESP_LOGD(TAG, "claw module opened");
    return 1;
}
