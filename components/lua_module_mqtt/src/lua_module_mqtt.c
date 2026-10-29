/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * `mqtt` Lua module (esp-mqtt, MQTT 3.1.1).
 *
 * Event model: the esp-mqtt task only copies events into a FreeRTOS queue;
 * Lua calls :next_event(ms) on the script task to consume one event per call.
 * No Lua state is ever touched from the mqtt task.
 */
#include "lauxlib.h"
#include "lua.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cap_lua.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lua_net_common.h"
#include "mqtt_client.h"

static const char *TAG = "lua_module_mqtt";

#define MQTT_MT             "mqtt.client"
#define MQTT_EVT_QUEUE_LEN  8
#define MQTT_MAX_TOPIC      192
#define MQTT_MAX_SUBS       8
#define MQTT_ERR_MAX        96
#define MQTT_POLL_SLICE_MS  50

typedef enum {
    MQTT_EVT_CONNECT = 0,
    MQTT_EVT_DISCONNECT,
    MQTT_EVT_MESSAGE,
    MQTT_EVT_ERROR,
} mqtt_evt_kind_t;

typedef struct {
    mqtt_evt_kind_t kind;
    char *topic;
    size_t topic_len;
    uint8_t *payload;
    size_t payload_len;
    uint8_t qos;
    bool truncated;
} mqtt_evt_t;

typedef struct {
    char topic[MQTT_MAX_TOPIC];
    int qos;
} mqtt_sub_t;

typedef struct {
    SemaphoreHandle_t lock;
    SemaphoreHandle_t wait;
    QueueHandle_t queue;
    esp_mqtt_client_handle_t client;
    bool started;
    bool connected;
    bool closed;
    bool auto_reconnect;
    bool budget_socket;
    bool budget_tls;
    int max_payload;
    char last_error[MQTT_ERR_MAX];
    unsigned dropped;
    mqtt_sub_t subs[MQTT_MAX_SUBS];
    size_t sub_count;
    /* MQTT task only */
    char *asm_topic;
    size_t asm_topic_len;
    uint8_t *asm_buf;
    size_t asm_len;
    size_t asm_total;
    uint8_t asm_qos;
    bool asm_discard;
    /* owned config strings */
    char *uri;
    char *client_id;
    char *username;
    char *password;
    char *ca_pem;
    char *will_topic;
    char *will_payload;
} lua_mqtt_conn_t;

typedef struct {
    lua_mqtt_conn_t *conn;
} lua_mqtt_ud_t;

static void mqtt_free_evt(mqtt_evt_t *evt)
{
    if (!evt) {
        return;
    }
    free(evt->topic);
    free(evt->payload);
    free(evt);
}

static void mqtt_set_error(lua_mqtt_conn_t *conn, const char *msg)
{
    if (!conn->lock) {
        return;
    }
    xSemaphoreTake(conn->lock, portMAX_DELAY);
    strlcpy(conn->last_error, msg ? msg : "mqtt error", sizeof(conn->last_error));
    xSemaphoreGive(conn->lock);
}

static void mqtt_enqueue(lua_mqtt_conn_t *conn, mqtt_evt_t *evt)
{
    if (!conn->queue || !evt) {
        mqtt_free_evt(evt);
        return;
    }
    if (xQueueSend(conn->queue, &evt, 0) == pdTRUE) {
        return;
    }
    mqtt_evt_t *old = NULL;
    if (xQueueReceive(conn->queue, &old, 0) == pdTRUE) {
        mqtt_free_evt(old);
    }
    if (xQueueSend(conn->queue, &evt, 0) != pdTRUE) {
        mqtt_free_evt(evt);
    }
    if (conn->lock) {
        xSemaphoreTake(conn->lock, portMAX_DELAY);
        conn->dropped++;
        xSemaphoreGive(conn->lock);
    }
}

static void mqtt_enqueue_simple(lua_mqtt_conn_t *conn, mqtt_evt_kind_t kind)
{
    mqtt_evt_t *evt = calloc(1, sizeof(*evt));
    if (!evt) {
        return;
    }
    evt->kind = kind;
    mqtt_enqueue(conn, evt);
}

static void mqtt_asm_reset(lua_mqtt_conn_t *conn)
{
    free(conn->asm_topic);
    conn->asm_topic = NULL;
    conn->asm_topic_len = 0;
    free(conn->asm_buf);
    conn->asm_buf = NULL;
    conn->asm_len = 0;
    conn->asm_total = 0;
    conn->asm_discard = false;
    conn->asm_qos = 0;
}

/* Runs on the esp-mqtt task. Only queue operations and copies. */
static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)base;
    lua_mqtt_conn_t *conn = arg;
    esp_mqtt_event_t *event = data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        if (conn->lock) {
            xSemaphoreTake(conn->lock, portMAX_DELAY);
            conn->connected = true;
            xSemaphoreGive(conn->lock);
        }
        xSemaphoreGive(conn->wait);
        mqtt_enqueue_simple(conn, MQTT_EVT_CONNECT);
        break;
    }
    case MQTT_EVENT_DISCONNECTED: {
        if (conn->lock) {
            xSemaphoreTake(conn->lock, portMAX_DELAY);
            conn->connected = false;
            xSemaphoreGive(conn->lock);
        }
        xSemaphoreGive(conn->wait);
        mqtt_enqueue_simple(conn, MQTT_EVT_DISCONNECT);
        break;
    }
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "subscribed msg_id=%d", event ? event->msg_id : -1);
        break;
    case MQTT_EVENT_ERROR:
        mqtt_set_error(conn, "mqtt error");
        xSemaphoreGive(conn->wait);
        mqtt_enqueue_simple(conn, MQTT_EVT_ERROR);
        break;
    case MQTT_EVENT_DATA: {
        if (!event) {
            break;
        }
        if (event->current_data_offset == 0) {
            ESP_LOGD(TAG, "data topic=%.*s len=%d total=%d",
                     event->topic_len, event->topic ? event->topic : "",
                     event->data_len, event->total_data_len);
        }
        if (event->current_data_offset == 0) {
            mqtt_asm_reset(conn);
            size_t topic_len = event->topic_len > 0 ? (size_t)event->topic_len : 0;
            if (event->topic && topic_len > 0) {
                conn->asm_topic = calloc(1, topic_len + 1);
                if (conn->asm_topic) {
                    memcpy(conn->asm_topic, event->topic, topic_len);
                    conn->asm_topic_len = topic_len;
                }
            }
            conn->asm_total = event->total_data_len > 0 ?
                              (size_t)event->total_data_len : (size_t)event->data_len;
            conn->asm_qos = (uint8_t)event->qos;
            conn->asm_discard = conn->asm_total > (size_t)conn->max_payload;
            if (!conn->asm_discard && conn->asm_total > 0) {
                conn->asm_buf = malloc(conn->asm_total);
                if (!conn->asm_buf) {
                    conn->asm_discard = true;
                }
            }
        }
        if (!conn->asm_topic && event->topic && event->topic_len > 0) {
            /* Topic may only arrive with the first fragment; keep a copy when
             * it appears later (defensive). */
            conn->asm_topic = calloc(1, (size_t)event->topic_len + 1);
            if (conn->asm_topic) {
                memcpy(conn->asm_topic, event->topic, (size_t)event->topic_len);
                conn->asm_topic_len = (size_t)event->topic_len;
            }
        }
        if (!conn->asm_discard && conn->asm_buf && event->data && event->data_len > 0) {
            size_t off = (size_t)event->current_data_offset;
            size_t len = (size_t)event->data_len;
            if (off + len <= conn->asm_total) {
                memcpy(conn->asm_buf + off, event->data, len);
                conn->asm_len = off + len;
            } else {
                conn->asm_discard = true;
            }
        }
        size_t done = (size_t)event->current_data_offset + (size_t)event->data_len;
        if (done >= conn->asm_total) {
            mqtt_evt_t *evt = calloc(1, sizeof(*evt));
            if (evt) {
                evt->kind = MQTT_EVT_MESSAGE;
                evt->qos = conn->asm_qos;
                evt->truncated = conn->asm_discard;
                if (conn->asm_topic) {
                    evt->topic = strdup(conn->asm_topic);
                    evt->topic_len = conn->asm_topic_len;
                }
                if (!conn->asm_discard && conn->asm_buf) {
                    evt->payload = malloc(conn->asm_len);
                    if (evt->payload) {
                        memcpy(evt->payload, conn->asm_buf, conn->asm_len);
                        evt->payload_len = conn->asm_len;
                    }
                }
                mqtt_enqueue(conn, evt);
            }
            mqtt_asm_reset(conn);
        }
        break;
    }
    default:
        break;
    }
}

static lua_mqtt_conn_t *mqtt_check(lua_State *L, int index)
{
    lua_mqtt_ud_t *ud = luaL_checkudata(L, index, MQTT_MT);
    if (!ud || !ud->conn) {
        luaL_error(L, "mqtt client is closed");
        return NULL;
    }
    return ud->conn;
}

static void mqtt_stop(lua_mqtt_conn_t *conn)
{
    if (!conn->client) {
        return;
    }
    if (conn->started) {
        (void)esp_mqtt_client_stop(conn->client);
        conn->started = false;
    }
    if (conn->lock) {
        xSemaphoreTake(conn->lock, portMAX_DELAY);
        conn->connected = false;
        xSemaphoreGive(conn->lock);
    }
}

static void mqtt_drain(lua_mqtt_conn_t *conn)
{
    if (!conn->queue) {
        return;
    }
    mqtt_evt_t *evt = NULL;
    while (xQueueReceive(conn->queue, &evt, 0) == pdTRUE) {
        mqtt_free_evt(evt);
    }
}

static void mqtt_release(lua_State *L, lua_mqtt_conn_t *conn)
{
    if (!conn || conn->closed) {
        return;
    }
    conn->closed = true;
    mqtt_stop(conn);
    if (conn->client) {
        esp_mqtt_client_destroy(conn->client);
        conn->client = NULL;
    }
    mqtt_drain(conn);
    if (conn->budget_tls) {
        lua_net_budget_release(L, LUA_NET_KIND_TLS);
        conn->budget_tls = false;
    }
    if (conn->budget_socket) {
        lua_net_budget_release(L, LUA_NET_KIND_SOCKET);
        conn->budget_socket = false;
    }
}

static int mqtt_close(lua_State *L)
{
    lua_mqtt_ud_t *ud = luaL_checkudata(L, 1, MQTT_MT);
    if (ud && ud->conn) {
        mqtt_release(L, ud->conn);
    }
    return 0;
}

static int mqtt_gc(lua_State *L)
{
    lua_mqtt_ud_t *ud = luaL_testudata(L, 1, MQTT_MT);
    if (ud && ud->conn) {
        lua_mqtt_conn_t *conn = ud->conn;
        mqtt_release(L, conn);
        mqtt_asm_reset(conn);
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
        free(conn->client_id);
        free(conn->username);
        free(conn->password);
        free(conn->ca_pem);
        free(conn->will_topic);
        free(conn->will_payload);
        free(conn);
        ud->conn = NULL;
    }
    return 0;
}

/* Wait for CONNECTED/ERROR/DISCONNECT. Returns 1 connected, 0 failed, -1
 * stopped/timeout (reason in *err). */
static int mqtt_wait_connected(lua_State *L, lua_mqtt_conn_t *conn,
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
                             pdMS_TO_TICKS(remaining > MQTT_POLL_SLICE_MS ?
                                           MQTT_POLL_SLICE_MS : remaining));
    }
}

static void mqtt_resubscribe(lua_mqtt_conn_t *conn)
{
    for (size_t i = 0; i < conn->sub_count; i++) {
        if (conn->client) {
            (void)esp_mqtt_client_subscribe(conn->client, conn->subs[i].topic,
                                            conn->subs[i].qos);
        }
    }
}

static int mqtt_connect(lua_State *L)
{
    lua_mqtt_conn_t *conn = mqtt_check(L, 1);
    int timeout_ms = (int)luaL_optinteger(L, 2, 10000);
    if (timeout_ms <= 0) {
        timeout_ms = 10000;
    }
    if (conn->closed) {
        lua_pushnil(L);
        lua_pushliteral(L, "closed");
        return 2;
    }
    if (!conn->started) {
        esp_err_t err = esp_mqtt_client_start(conn->client);
        if (err != ESP_OK) {
            lua_pushnil(L);
            lua_pushliteral(L, "start failed");
            return 2;
        }
        conn->started = true;
    }

    const char *err = NULL;
    int wait = mqtt_wait_connected(L, conn, timeout_ms, &err);
    if (wait == 1) {
        mqtt_resubscribe(conn);
        lua_pushboolean(L, 1);
        return 1;
    }
    if (wait < 0) {
        if (!conn->auto_reconnect) {
            mqtt_stop(conn);
        }
        lua_pushnil(L);
        lua_pushstring(L, err ? err : "timeout");
        return 2;
    }
    return 0; /* not reached */
}

static int mqtt_publish(lua_State *L)
{
    lua_mqtt_conn_t *conn = mqtt_check(L, 1);
    const char *topic = luaL_checkstring(L, 2);
    size_t payload_len = 0;
    const char *payload = luaL_checklstring(L, 3, &payload_len);
    int qos = 0;
    bool retain = false;
    if (lua_istable(L, 4)) {
        lua_getfield(L, 4, "qos");
        if (lua_isinteger(L, -1)) {
            qos = (int)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);
        lua_getfield(L, 4, "retain");
        retain = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }
    if (qos < 0 || qos > 2) {
        return luaL_error(L, "qos must be 0..2");
    }

    bool connected;
    xSemaphoreTake(conn->lock, portMAX_DELAY);
    connected = conn->connected;
    xSemaphoreGive(conn->lock);
    if (!connected) {
        lua_pushnil(L);
        lua_pushliteral(L, "not connected");
        return 2;
    }
    int msg_id = esp_mqtt_client_publish(conn->client, topic, payload,
                                         (int)payload_len, qos, retain ? 1 : 0);
    if (msg_id < 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "publish failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int mqtt_subscribe(lua_State *L)
{
    lua_mqtt_conn_t *conn = mqtt_check(L, 1);
    const char *topic = luaL_checkstring(L, 2);
    int qos = (int)luaL_optinteger(L, 3, 0);
    if (strlen(topic) >= MQTT_MAX_TOPIC) {
        return luaL_error(L, "topic too long");
    }
    bool connected;
    xSemaphoreTake(conn->lock, portMAX_DELAY);
    connected = conn->connected;
    xSemaphoreGive(conn->lock);
    if (!connected) {
        lua_pushnil(L);
        lua_pushliteral(L, "not connected");
        return 2;
    }
    int msg_id = esp_mqtt_client_subscribe(conn->client, topic, qos);
    if (msg_id < 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "subscribe failed");
        return 2;
    }
    for (size_t i = 0; i < conn->sub_count; i++) {
        if (strcmp(conn->subs[i].topic, topic) == 0) {
            conn->subs[i].qos = qos;
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    if (conn->sub_count < MQTT_MAX_SUBS) {
        strlcpy(conn->subs[conn->sub_count].topic, topic,
                sizeof(conn->subs[conn->sub_count].topic));
        conn->subs[conn->sub_count].qos = qos;
        conn->sub_count++;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int mqtt_unsubscribe(lua_State *L)
{
    lua_mqtt_conn_t *conn = mqtt_check(L, 1);
    const char *topic = luaL_checkstring(L, 2);
    if (conn->client) {
        (void)esp_mqtt_client_unsubscribe(conn->client, topic);
    }
    for (size_t i = 0; i < conn->sub_count; i++) {
        if (strcmp(conn->subs[i].topic, topic) == 0) {
            conn->subs[i] = conn->subs[conn->sub_count - 1];
            conn->sub_count--;
            break;
        }
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int mqtt_connected(lua_State *L)
{
    lua_mqtt_conn_t *conn = mqtt_check(L, 1);
    bool connected;
    xSemaphoreTake(conn->lock, portMAX_DELAY);
    connected = conn->connected;
    xSemaphoreGive(conn->lock);
    lua_pushboolean(L, connected);
    return 1;
}

static void mqtt_push_event(lua_State *L, lua_mqtt_conn_t *conn, mqtt_evt_t *evt)
{
    lua_newtable(L);
    switch (evt->kind) {
    case MQTT_EVT_CONNECT:
        lua_pushliteral(L, "connect");
        lua_setfield(L, -2, "type");
        mqtt_resubscribe(conn);
        break;
    case MQTT_EVT_DISCONNECT:
        lua_pushliteral(L, "disconnect");
        lua_setfield(L, -2, "type");
        break;
    case MQTT_EVT_ERROR:
        lua_pushliteral(L, "error");
        lua_setfield(L, -2, "type");
        lua_pushstring(L, conn->last_error);
        lua_setfield(L, -2, "message");
        break;
    case MQTT_EVT_MESSAGE:
        lua_pushliteral(L, "message");
        lua_setfield(L, -2, "type");
        lua_pushlstring(L, evt->topic ? evt->topic : "", evt->topic_len);
        lua_setfield(L, -2, "topic");
        lua_pushlstring(L, evt->payload ? (const char *)evt->payload : "",
                        evt->payload_len);
        lua_setfield(L, -2, "payload");
        lua_pushinteger(L, evt->qos);
        lua_setfield(L, -2, "qos");
        lua_pushboolean(L, evt->truncated);
        lua_setfield(L, -2, "truncated");
        break;
    default:
        break;
    }
}

static int mqtt_next_event(lua_State *L)
{
    lua_mqtt_conn_t *conn = mqtt_check(L, 1);
    int timeout_ms = (int)luaL_optinteger(L, 2, 0);
    int64_t deadline = lua_net_deadline(L, timeout_ms);
    mqtt_evt_t *evt = NULL;

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
                          pdMS_TO_TICKS(remaining > MQTT_POLL_SLICE_MS ?
                                        MQTT_POLL_SLICE_MS : remaining)) == pdTRUE) {
            break;
        }
    }

    mqtt_push_event(L, conn, evt);
    mqtt_free_evt(evt);
    return 1;
}

static int mqtt_last_error(lua_State *L)
{
    lua_mqtt_conn_t *conn = mqtt_check(L, 1);
    lua_pushstring(L, conn->last_error);
    return 1;
}

static int mqtt_state(lua_State *L)
{
    lua_mqtt_conn_t *conn = mqtt_check(L, 1);
    if (conn->closed) {
        lua_pushliteral(L, "closed");
        return 1;
    }
    bool connected;
    xSemaphoreTake(conn->lock, portMAX_DELAY);
    connected = conn->connected;
    xSemaphoreGive(conn->lock);
    lua_pushstring(L, connected ? "connected" : "disconnected");
    return 1;
}

static int mqtt_dropped(lua_State *L)
{
    lua_mqtt_conn_t *conn = mqtt_check(L, 1);
    unsigned dropped;
    xSemaphoreTake(conn->lock, portMAX_DELAY);
    dropped = conn->dropped;
    xSemaphoreGive(conn->lock);
    lua_pushinteger(L, dropped);
    return 1;
}

static int mqtt_disconnect(lua_State *L)
{
    lua_mqtt_conn_t *conn = mqtt_check(L, 1);
    mqtt_stop(conn);
    return 0;
}

static char *dup_opt_str(lua_State *L, int table, const char *key)
{
    lua_getfield(L, table, key);
    char *copy = NULL;
    if (lua_isstring(L, -1)) {
        copy = strdup(lua_tostring(L, -1));
    }
    lua_pop(L, 1);
    return copy;
}

static char *dup_lit(const char *value)
{
    return value ? strdup(value) : NULL;
}

static int mqtt_new(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "uri");
    if (!lua_isstring(L, -1)) {
        return luaL_error(L, "mqtt.new: uri is required");
    }
    const char *uri = lua_tostring(L, -1);

    lua_mqtt_conn_t *conn = calloc(1, sizeof(*conn));
    if (!conn) {
        return luaL_error(L, "out of memory");
    }
    conn->uri = dup_lit(uri);
    conn->client_id = dup_opt_str(L, 1, "client_id");
    conn->username = dup_opt_str(L, 1, "username");
    conn->password = dup_opt_str(L, 1, "password");
    conn->ca_pem = dup_opt_str(L, 1, "ca_pem");
    conn->lock = xSemaphoreCreateMutex();
    conn->wait = xSemaphoreCreateBinary();
    conn->queue = xQueueCreate(MQTT_EVT_QUEUE_LEN, sizeof(mqtt_evt_t *));

    lua_getfield(L, 1, "max_payload");
    int max_payload = lua_isinteger(L, -1) ? (int)lua_tointeger(L, -1) : 16384;
    lua_pop(L, 1);
    if (max_payload < 512) {
        max_payload = 512;
    }
    if (max_payload > 65536) {
        max_payload = 65536;
    }
    conn->max_payload = max_payload;

    lua_getfield(L, 1, "reconnect_ms");
    int reconnect_ms = lua_isinteger(L, -1) ? (int)lua_tointeger(L, -1) : 0;
    lua_pop(L, 1);
    conn->auto_reconnect = reconnect_ms > 0;

    lua_getfield(L, 1, "keepalive_s");
    int keepalive = lua_isinteger(L, -1) ? (int)lua_tointeger(L, -1) : 60;
    lua_pop(L, 1);

    lua_getfield(L, 1, "clean_session");
    bool clean_session = lua_isnil(L, -1) ? true : lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "insecure");
    bool insecure = lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "will");
    bool has_will = lua_istable(L, -1);
    if (has_will) {
        conn->will_topic = dup_opt_str(L, -1, "topic");
        conn->will_payload = dup_opt_str(L, -1, "payload");
    }
    lua_pop(L, 1);

    if (!conn->uri || !conn->lock || !conn->wait || !conn->queue) {
        /* gc will free whatever was allocated */
        lua_mqtt_ud_t *ud = lua_newuserdata(L, sizeof(*ud));
        ud->conn = conn;
        luaL_setmetatable(L, MQTT_MT);
        mqtt_release(L, conn);
        return luaL_error(L, "mqtt.new: out of memory");
    }

    bool is_tls = strncasecmp(conn->uri, "mqtts://", 8) == 0;
    const char *err = NULL;
    if (!lua_net_budget_acquire(L, LUA_NET_KIND_SOCKET, &err)) {
        lua_mqtt_ud_t *ud = lua_newuserdata(L, sizeof(*ud));
        ud->conn = conn;
        luaL_setmetatable(L, MQTT_MT);
        mqtt_release(L, conn);
        lua_pushnil(L);
        lua_pushstring(L, err);
        return 2;
    }
    conn->budget_socket = true;
    if (is_tls) {
        if (!lua_net_budget_acquire(L, LUA_NET_KIND_TLS, &err)) {
            lua_mqtt_ud_t *ud = lua_newuserdata(L, sizeof(*ud));
            ud->conn = conn;
            luaL_setmetatable(L, MQTT_MT);
            mqtt_release(L, conn);
            lua_pushnil(L);
            lua_pushstring(L, err);
            return 2;
        }
        conn->budget_tls = true;
    }

    esp_mqtt_client_config_t config = {0};
    config.broker.address.uri = conn->uri;
    config.credentials.client_id = conn->client_id;
    config.credentials.username = conn->username;
    config.credentials.authentication.password = conn->password;
    config.session.keepalive = keepalive > 0 ? keepalive : 60;
    config.session.disable_keepalive = keepalive <= 0;
    config.session.disable_clean_session = !clean_session;
    config.network.timeout_ms = 3000;
    config.network.reconnect_timeout_ms = reconnect_ms > 0 ? reconnect_ms : 1000;
    config.network.disable_auto_reconnect = !conn->auto_reconnect;
    config.buffer.size = 1024;
    config.buffer.out_size = 1024;
    config.task.stack_size = 6144;

    if (has_will && conn->will_topic) {
        config.session.last_will.topic = conn->will_topic;
        config.session.last_will.msg = conn->will_payload;
        config.session.last_will.msg_len = conn->will_payload ?
                                           (int)strlen(conn->will_payload) : 0;
        /* qos/retain read again from the Lua table below */
        lua_getfield(L, 1, "will");
        lua_getfield(L, -1, "qos");
        config.session.last_will.qos = lua_isinteger(L, -1) ?
                                       (int)lua_tointeger(L, -1) : 0;
        lua_pop(L, 1);
        lua_getfield(L, -1, "retain");
        config.session.last_will.retain = lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_pop(L, 1);
    }
    if (is_tls) {
        if (insecure) {
            config.broker.verification.skip_cert_common_name_check = true;
        } else if (conn->ca_pem) {
            config.broker.verification.certificate = conn->ca_pem;
            config.broker.verification.certificate_len = strlen(conn->ca_pem) + 1;
        } else {
            config.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        }
    }

    conn->client = esp_mqtt_client_init(&config);
    if (!conn->client) {
        lua_mqtt_ud_t *ud = lua_newuserdata(L, sizeof(*ud));
        ud->conn = conn;
        luaL_setmetatable(L, MQTT_MT);
        mqtt_release(L, conn);
        lua_pushnil(L);
        lua_pushliteral(L, "mqtt init failed");
        return 2;
    }
    esp_mqtt_client_register_event(conn->client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, conn);

    lua_mqtt_ud_t *ud = lua_newuserdata(L, sizeof(*ud));
    ud->conn = conn;
    luaL_setmetatable(L, MQTT_MT);
    return 1;
}

static const luaL_Reg mqtt_methods[] = {
    {"connect", mqtt_connect},
    {"publish", mqtt_publish},
    {"subscribe", mqtt_subscribe},
    {"unsubscribe", mqtt_unsubscribe},
    {"connected", mqtt_connected},
    {"next_event", mqtt_next_event},
    {"last_error", mqtt_last_error},
    {"state", mqtt_state},
    {"dropped", mqtt_dropped},
    {"disconnect", mqtt_disconnect},
    {"close", mqtt_close},
    {NULL, NULL},
};

int luaopen_mqtt(lua_State *L)
{
    if (luaL_newmetatable(L, MQTT_MT)) {
        lua_pushcfunction(L, mqtt_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushcfunction(L, mqtt_close);
        lua_setfield(L, -2, "__close");
        lua_newtable(L);
        luaL_setfuncs(L, mqtt_methods, 0);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, mqtt_new);
    lua_setfield(L, -2, "new");
    return 1;
}

esp_err_t lua_module_mqtt_register(void)
{
    esp_err_t err = cap_lua_register_module("mqtt", luaopen_mqtt);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Lua mqtt module registered");
    }
    return err;
}
