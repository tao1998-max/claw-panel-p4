/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * `websocket` Lua module (esp_websocket_client).
 *
 * Event model: the websocket task only copies fragments into a FreeRTOS
 * queue; Lua calls :next_event(ms) on the script task. The Lua state is
 * never touched from the websocket task.
 */
#include "lauxlib.h"
#include "lua.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cap_lua.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lua_net_common.h"

static const char *TAG = "lua_module_websocket";

#define WS_MT              "websocket.client"
#define WS_EVT_QUEUE_LEN   8
#define WS_ERR_MAX         96
#define WS_POLL_SLICE_MS   50
#define WS_OPCODE_TEXT     0x01
#define WS_OPCODE_BINARY   0x02
#define WS_OPCODE_CONT     0x00
#define WS_OPCODE_CLOSE    0x08

typedef enum {
    WS_EVT_CONNECT = 0,
    WS_EVT_DISCONNECT,
    WS_EVT_DATA,
    WS_EVT_ERROR,
} ws_evt_kind_t;

typedef struct {
    ws_evt_kind_t kind;
    char *data;
    size_t len;
    bool binary;
    bool truncated;
} ws_evt_t;

typedef struct {
    SemaphoreHandle_t lock;
    SemaphoreHandle_t wait;
    QueueHandle_t queue;
    esp_websocket_client_handle_t client;
    bool started;
    bool connected;
    bool closed;
    bool auto_reconnect;
    bool budget_socket;
    bool budget_tls;
    int max_payload;
    int close_code;
    char last_error[WS_ERR_MAX];
    unsigned dropped;
    /* websocket task only */
    uint8_t *asm_buf;
    size_t asm_len;
    size_t asm_total;
    uint8_t asm_opcode;
    bool asm_discard;
    /* owned config strings (the client does not copy them) */
    char *uri;
    char *headers;
    char *ca_pem;
    char *subprotocol;
} lua_ws_conn_t;

typedef struct {
    lua_ws_conn_t *conn;
} lua_ws_ud_t;

static void ws_free_evt(ws_evt_t *evt)
{
    if (!evt) {
        return;
    }
    free(evt->data);
    free(evt);
}

static void ws_enqueue(lua_ws_conn_t *conn, ws_evt_t *evt)
{
    if (!conn->queue || !evt) {
        ws_free_evt(evt);
        return;
    }
    if (xQueueSend(conn->queue, &evt, 0) == pdTRUE) {
        return;
    }
    ws_evt_t *old = NULL;
    if (xQueueReceive(conn->queue, &old, 0) == pdTRUE) {
        ws_free_evt(old);
    }
    if (xQueueSend(conn->queue, &evt, 0) != pdTRUE) {
        ws_free_evt(evt);
    }
    if (conn->lock) {
        xSemaphoreTake(conn->lock, portMAX_DELAY);
        conn->dropped++;
        xSemaphoreGive(conn->lock);
    }
}

static void ws_enqueue_simple(lua_ws_conn_t *conn, ws_evt_kind_t kind)
{
    ws_evt_t *evt = calloc(1, sizeof(*evt));
    if (!evt) {
        return;
    }
    evt->kind = kind;
    ws_enqueue(conn, evt);
}

static void ws_asm_reset(lua_ws_conn_t *conn)
{
    free(conn->asm_buf);
    conn->asm_buf = NULL;
    conn->asm_len = 0;
    conn->asm_total = 0;
    conn->asm_opcode = 0;
    conn->asm_discard = false;
}

static void ws_set_error(lua_ws_conn_t *conn, const char *msg)
{
    if (!conn->lock) {
        return;
    }
    xSemaphoreTake(conn->lock, portMAX_DELAY);
    strlcpy(conn->last_error, msg ? msg : "websocket error",
            sizeof(conn->last_error));
    xSemaphoreGive(conn->lock);
}

/* Runs on the websocket task. Only copies + queue operations. */
static void ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)base;
    lua_ws_conn_t *conn = arg;
    esp_websocket_event_data_t *event = data;

    switch ((esp_websocket_event_id_t)id) {
    case WEBSOCKET_EVENT_CONNECTED:
        if (conn->lock) {
            xSemaphoreTake(conn->lock, portMAX_DELAY);
            conn->connected = true;
            xSemaphoreGive(conn->lock);
        }
        xSemaphoreGive(conn->wait);
        ws_enqueue_simple(conn, WS_EVT_CONNECT);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        if (conn->lock) {
            xSemaphoreTake(conn->lock, portMAX_DELAY);
            conn->connected = false;
            xSemaphoreGive(conn->lock);
        }
        xSemaphoreGive(conn->wait);
        ws_enqueue_simple(conn, WS_EVT_DISCONNECT);
        break;
    case WEBSOCKET_EVENT_ERROR:
        ws_set_error(conn, "websocket error");
        xSemaphoreGive(conn->wait);
        ws_enqueue_simple(conn, WS_EVT_ERROR);
        break;
    case WEBSOCKET_EVENT_DATA: {
        if (!event || !event->data_ptr || event->data_len <= 0) {
            break;
        }
        if (event->op_code >= 0x08) {
            /* Control frames (close/ping/pong) are handled by the client. */
            break;
        }
        if (event->payload_offset == 0) {
            ws_asm_reset(conn);
            conn->asm_total = event->payload_len > 0 ?
                              (size_t)event->payload_len : (size_t)event->data_len;
            conn->asm_opcode = event->op_code;
            conn->asm_discard = conn->asm_total > (size_t)conn->max_payload;
            if (!conn->asm_discard) {
                conn->asm_buf = malloc(conn->asm_total);
                if (!conn->asm_buf) {
                    conn->asm_discard = true;
                }
            }
            if (event->op_code == WS_OPCODE_CONT) {
                conn->asm_opcode = WS_OPCODE_TEXT;
            }
        }
        if (!conn->asm_discard && conn->asm_buf) {
            size_t off = (size_t)event->payload_offset;
            size_t len = (size_t)event->data_len;
            if (off + len <= conn->asm_total) {
                memcpy(conn->asm_buf + off, event->data_ptr, len);
                conn->asm_len = off + len;
            } else {
                conn->asm_discard = true;
            }
        }
        size_t done = (size_t)event->payload_offset + (size_t)event->data_len;
        if (done >= conn->asm_total) {
            ws_evt_t *evt = calloc(1, sizeof(*evt));
            if (evt) {
                evt->kind = WS_EVT_DATA;
                evt->binary = conn->asm_opcode == WS_OPCODE_BINARY;
                evt->truncated = conn->asm_discard;
                if (!conn->asm_discard && conn->asm_buf) {
                    evt->data = malloc(conn->asm_len ? conn->asm_len : 1);
                    if (evt->data) {
                        memcpy(evt->data, conn->asm_buf, conn->asm_len);
                        evt->len = conn->asm_len;
                    }
                }
                ws_enqueue(conn, evt);
            }
            ws_asm_reset(conn);
        }
        break;
    }
    default:
        break;
    }
}

static lua_ws_conn_t *ws_check(lua_State *L, int index)
{
    lua_ws_ud_t *ud = luaL_checkudata(L, index, WS_MT);
    if (!ud || !ud->conn) {
        luaL_error(L, "websocket client is closed");
        return NULL;
    }
    return ud->conn;
}

static void ws_drain(lua_ws_conn_t *conn)
{
    if (!conn->queue) {
        return;
    }
    ws_evt_t *evt = NULL;
    while (xQueueReceive(conn->queue, &evt, 0) == pdTRUE) {
        ws_free_evt(evt);
    }
}

static void ws_release(lua_State *L, lua_ws_conn_t *conn)
{
    if (!conn || conn->closed) {
        return;
    }
    conn->closed = true;
    if (conn->client) {
        if (conn->started) {
            (void)esp_websocket_client_stop(conn->client);
            conn->started = false;
        }
        esp_websocket_client_destroy(conn->client);
        conn->client = NULL;
    }
    conn->connected = false;
    ws_drain(conn);
    if (conn->budget_tls) {
        lua_net_budget_release(L, LUA_NET_KIND_TLS);
        conn->budget_tls = false;
    }
    if (conn->budget_socket) {
        lua_net_budget_release(L, LUA_NET_KIND_SOCKET);
        conn->budget_socket = false;
    }
}

static int ws_close(lua_State *L)
{
    lua_ws_ud_t *ud = luaL_checkudata(L, 1, WS_MT);
    if (ud && ud->conn) {
        lua_ws_conn_t *conn = ud->conn;
        if (conn->client && conn->started) {
            int code = (int)luaL_optinteger(L, 2, 0);
            (void)code;
            (void)esp_websocket_client_close(conn->client, pdMS_TO_TICKS(1000));
        }
        ws_release(L, conn);
    }
    return 0;
}

static int ws_gc(lua_State *L)
{
    lua_ws_ud_t *ud = luaL_testudata(L, 1, WS_MT);
    if (ud && ud->conn) {
        lua_ws_conn_t *conn = ud->conn;
        ws_release(L, conn);
        ws_asm_reset(conn);
        if (conn->queue) {
            vQueueDelete(conn->queue);
        }
        if (conn->wait) {
            vSemaphoreDelete(conn->wait);
        }
        if (conn->lock) {
            vSemaphoreDelete(conn->lock);
        }
        free(conn->uri);
        free(conn->headers);
        free(conn->ca_pem);
        free(conn->subprotocol);
        free(conn);
        ud->conn = NULL;
    }
    return 0;
}

static int ws_wait_connected(lua_State *L, lua_ws_conn_t *conn,
                             int timeout_ms, const char **err)
{
    int64_t deadline = lua_net_deadline(L, timeout_ms);
    while (true) {
        bool connected;
        xSemaphoreTake(conn->lock, portMAX_DELAY);
        connected = conn->connected;
        xSemaphoreGive(conn->lock);
        if (connected) {
            return 1;
        }
        if (lua_net_stopped(L)) {
            *err = "stopped";
            return -1;
        }
        int remaining = lua_net_remaining_ms(deadline);
        if (remaining == 0) {
            *err = "timeout";
            return -1;
        }
        (void)xSemaphoreTake(conn->wait,
                             pdMS_TO_TICKS(remaining > WS_POLL_SLICE_MS ?
                                           WS_POLL_SLICE_MS : remaining));
    }
}

static int ws_connect(lua_State *L)
{
    const char *url = luaL_checkstring(L, 1);
    int timeout_ms = 5000;
    int reconnect_ms = 0;
    int buffer_size = 2048;
    int task_stack = 6144;
    int ping_interval_s = 10;
    int pingpong_timeout_s = 15;
    int max_payload = 8192;
    bool insecure = false;

    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "timeout_ms");
        if (lua_isinteger(L, -1)) {
            timeout_ms = (int)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "reconnect_ms");
        if (lua_isinteger(L, -1)) {
            reconnect_ms = (int)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "buffer_size");
        if (lua_isinteger(L, -1)) {
            buffer_size = (int)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "task_stack");
        if (lua_isinteger(L, -1)) {
            task_stack = (int)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "ping_interval_s");
        if (lua_isinteger(L, -1)) {
            ping_interval_s = (int)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "pingpong_timeout_s");
        if (lua_isinteger(L, -1)) {
            pingpong_timeout_s = (int)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "max_payload");
        if (lua_isinteger(L, -1)) {
            max_payload = (int)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "insecure");
        insecure = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }
    if (timeout_ms <= 0) {
        timeout_ms = 5000;
    }
    if (buffer_size < 512 || buffer_size > 16384) {
        buffer_size = 2048;
    }
    if (task_stack < 4096 || task_stack > 32768) {
        task_stack = 6144;
    }
    if (max_payload < 512) {
        max_payload = 512;
    }
    if (max_payload > 65536) {
        max_payload = 65536;
    }
    if (ping_interval_s < 0) {
        ping_interval_s = 0;
    }
    if (pingpong_timeout_s < 0) {
        pingpong_timeout_s = 0;
    }

    lua_ws_conn_t *conn = calloc(1, sizeof(*conn));
    if (!conn) {
        return luaL_error(L, "out of memory");
    }
    conn->uri = strdup(url);
    conn->headers = NULL;
    conn->ca_pem = NULL;
    conn->subprotocol = NULL;
    conn->lock = xSemaphoreCreateMutex();
    conn->wait = xSemaphoreCreateBinary();
    conn->queue = xQueueCreate(WS_EVT_QUEUE_LEN, sizeof(ws_evt_t *));
    conn->max_payload = max_payload;
    conn->auto_reconnect = reconnect_ms > 0;

    if (lua_isnoneornil(L, 2) == 0) {
        lua_getfield(L, 2, "headers");
        if (lua_isstring(L, -1)) {
            conn->headers = strdup(lua_tostring(L, -1));
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "ca_pem");
        if (lua_isstring(L, -1)) {
            conn->ca_pem = strdup(lua_tostring(L, -1));
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "subprotocol");
        if (lua_isstring(L, -1)) {
            conn->subprotocol = strdup(lua_tostring(L, -1));
        }
        lua_pop(L, 1);
    }

    if (!conn->uri || !conn->lock || !conn->wait || !conn->queue) {
        lua_ws_ud_t *ud = lua_newuserdata(L, sizeof(*ud));
        ud->conn = conn;
        luaL_setmetatable(L, WS_MT);
        ws_release(L, conn);
        return luaL_error(L, "websocket.connect: out of memory");
    }

    bool is_tls = strncasecmp(conn->uri, "wss://", 6) == 0;
    const char *err = NULL;
    if (!lua_net_budget_acquire(L, LUA_NET_KIND_SOCKET, &err)) {
        lua_ws_ud_t *ud = lua_newuserdata(L, sizeof(*ud));
        ud->conn = conn;
        luaL_setmetatable(L, WS_MT);
        ws_release(L, conn);
        lua_pushnil(L);
        lua_pushstring(L, err);
        return 2;
    }
    conn->budget_socket = true;
    if (is_tls) {
        if (!lua_net_budget_acquire(L, LUA_NET_KIND_TLS, &err)) {
            lua_ws_ud_t *ud = lua_newuserdata(L, sizeof(*ud));
            ud->conn = conn;
            luaL_setmetatable(L, WS_MT);
            ws_release(L, conn);
            lua_pushnil(L);
            lua_pushstring(L, err);
            return 2;
        }
        conn->budget_tls = true;
    }

    esp_websocket_client_config_t config = {0};
    config.uri = conn->uri;
    config.headers = conn->headers;
    config.subprotocol = conn->subprotocol;
    config.buffer_size = buffer_size;
    config.task_stack = task_stack;
    config.network_timeout_ms = timeout_ms;
    config.reconnect_timeout_ms = reconnect_ms > 0 ? reconnect_ms : 1000;
    config.disable_auto_reconnect = !conn->auto_reconnect;
    config.ping_interval_sec = (size_t)ping_interval_s;
    config.pingpong_timeout_sec = pingpong_timeout_s;
    config.disable_pingpong_discon = false;
    if (is_tls) {
        if (insecure) {
            config.skip_cert_common_name_check = true;
        } else if (conn->ca_pem) {
            config.cert_pem = conn->ca_pem;
            config.cert_len = strlen(conn->ca_pem) + 1;
        } else {
            config.crt_bundle_attach = esp_crt_bundle_attach;
        }
    }

    conn->client = esp_websocket_client_init(&config);
    if (!conn->client) {
        lua_ws_ud_t *ud = lua_newuserdata(L, sizeof(*ud));
        ud->conn = conn;
        luaL_setmetatable(L, WS_MT);
        ws_release(L, conn);
        lua_pushnil(L);
        lua_pushliteral(L, "websocket init failed");
        return 2;
    }
    esp_websocket_register_events(conn->client, WEBSOCKET_EVENT_ANY,
                                  ws_event_handler, conn);

    esp_err_t start_err = esp_websocket_client_start(conn->client);
    if (start_err != ESP_OK) {
        lua_ws_ud_t *ud = lua_newuserdata(L, sizeof(*ud));
        ud->conn = conn;
        luaL_setmetatable(L, WS_MT);
        ws_release(L, conn);
        lua_pushnil(L);
        lua_pushliteral(L, "websocket start failed");
        return 2;
    }
    conn->started = true;

    if (ws_wait_connected(L, conn, timeout_ms, &err) != 1) {
        ws_release(L, conn);
        lua_pushnil(L);
        lua_pushstring(L, err ? err : "timeout");
        return 2;
    }

    lua_ws_ud_t *ud = lua_newuserdata(L, sizeof(*ud));
    ud->conn = conn;
    luaL_setmetatable(L, WS_MT);
    return 1;
}

static int ws_send(lua_State *L)
{
    lua_ws_conn_t *conn = ws_check(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    bool binary = false;
    int timeout_ms = 3000;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "binary");
        binary = lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "timeout_ms");
        if (lua_isinteger(L, -1)) {
            timeout_ms = (int)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);
    }
    if (timeout_ms <= 0) {
        timeout_ms = 3000;
    }
    if (!conn->client) {
        lua_pushnil(L);
        lua_pushliteral(L, "closed");
        return 2;
    }
    int sent = binary ?
               esp_websocket_client_send_bin(conn->client, data, (int)len,
                                             pdMS_TO_TICKS(timeout_ms)) :
               esp_websocket_client_send_text(conn->client, data, (int)len,
                                              pdMS_TO_TICKS(timeout_ms));
    if (sent < 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "send failed");
        return 2;
    }
    lua_pushinteger(L, sent);
    return 1;
}

static int ws_connected(lua_State *L)
{
    lua_ws_conn_t *conn = ws_check(L, 1);
    bool connected;
    xSemaphoreTake(conn->lock, portMAX_DELAY);
    connected = conn->connected;
    xSemaphoreGive(conn->lock);
    lua_pushboolean(L, connected);
    return 1;
}

static void ws_push_event(lua_State *L, lua_ws_conn_t *conn, ws_evt_t *evt)
{
    lua_newtable(L);
    switch (evt->kind) {
    case WS_EVT_CONNECT:
        lua_pushliteral(L, "connect");
        lua_setfield(L, -2, "type");
        break;
    case WS_EVT_DISCONNECT:
        lua_pushliteral(L, "disconnect");
        lua_setfield(L, -2, "type");
        break;
    case WS_EVT_ERROR:
        lua_pushliteral(L, "error");
        lua_setfield(L, -2, "type");
        lua_pushstring(L, conn->last_error);
        lua_setfield(L, -2, "message");
        break;
    case WS_EVT_DATA:
        lua_pushliteral(L, "data");
        lua_setfield(L, -2, "type");
        lua_pushlstring(L, evt->data ? evt->data : "", evt->len);
        lua_setfield(L, -2, "data");
        lua_pushboolean(L, evt->binary);
        lua_setfield(L, -2, "binary");
        lua_pushboolean(L, evt->truncated);
        lua_setfield(L, -2, "truncated");
        break;
    default:
        break;
    }
}

static int ws_next_event(lua_State *L)
{
    lua_ws_conn_t *conn = ws_check(L, 1);
    int timeout_ms = (int)luaL_optinteger(L, 2, 0);
    int64_t deadline = lua_net_deadline(L, timeout_ms);
    ws_evt_t *evt = NULL;

    while (true) {
        if (!conn->queue) {
            lua_pushnil(L);
            return 1;
        }
        if (xQueueReceive(conn->queue, &evt, 0) == pdTRUE) {
            break;
        }
        if (timeout_ms <= 0) {
            lua_pushnil(L);
            return 1;
        }
        if (lua_net_stopped(L)) {
            lua_pushnil(L);
            lua_pushliteral(L, "stopped");
            return 2;
        }
        int remaining = lua_net_remaining_ms(deadline);
        if (remaining == 0) {
            lua_pushnil(L);
            return 1;
        }
        if (xQueueReceive(conn->queue, &evt,
                          pdMS_TO_TICKS(remaining > WS_POLL_SLICE_MS ?
                                        WS_POLL_SLICE_MS : remaining)) == pdTRUE) {
            break;
        }
    }

    ws_push_event(L, conn, evt);
    ws_free_evt(evt);
    return 1;
}

static int ws_last_error(lua_State *L)
{
    lua_ws_conn_t *conn = ws_check(L, 1);
    lua_pushstring(L, conn->last_error);
    return 1;
}

static int ws_dropped(lua_State *L)
{
    lua_ws_conn_t *conn = ws_check(L, 1);
    unsigned dropped;
    xSemaphoreTake(conn->lock, portMAX_DELAY);
    dropped = conn->dropped;
    xSemaphoreGive(conn->lock);
    lua_pushinteger(L, dropped);
    return 1;
}

static const luaL_Reg ws_methods[] = {
    {"send", ws_send},
    {"connected", ws_connected},
    {"next_event", ws_next_event},
    {"last_error", ws_last_error},
    {"dropped", ws_dropped},
    {"close", ws_close},
    {NULL, NULL},
};

int luaopen_websocket(lua_State *L)
{
    if (luaL_newmetatable(L, WS_MT)) {
        lua_pushcfunction(L, ws_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushcfunction(L, ws_close);
        lua_setfield(L, -2, "__close");
        lua_newtable(L);
        luaL_setfuncs(L, ws_methods, 0);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, ws_connect);
    lua_setfield(L, -2, "connect");
    return 1;
}

esp_err_t lua_module_websocket_register(void)
{
    esp_err_t err = cap_lua_register_module("websocket", luaopen_websocket);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Lua websocket module registered");
    }
    return err;
}
