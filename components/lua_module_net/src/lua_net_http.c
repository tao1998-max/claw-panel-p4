/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * net.http.request: HTTP/1.1 client on top of esp_http_client, following the
 * abortable open/write/fetch_headers/read pattern proven in claw_http.c.
 * TLS, chunked decoding and redirects are handled by esp_http_client.
 */
#include "lua_module_net.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lua_net_http";

#define HTTP_HEADER_MAX      16
#define HTTP_HEADER_KEY_MAX  64
#define HTTP_HEADER_VAL_MAX  192
#define HTTP_HEADER_BYTES    8192
#define HTTP_BODY_DEFAULT    (64 * 1024)
#define HTTP_BODY_MAX        (1024 * 1024)
#define HTTP_IO_SLICE_MS     100
#define HTTP_READ_CHUNK      4096

typedef struct {
    char key[HTTP_HEADER_KEY_MAX];
    char value[HTTP_HEADER_VAL_MAX];
} http_header_t;

typedef struct {
    http_header_t headers[HTTP_HEADER_MAX];
    size_t header_count;
    size_t header_bytes;
    bool truncated;
} http_ctx_t;

static void http_ctx_reset(http_ctx_t *ctx)
{
    ctx->header_count = 0;
    ctx->header_bytes = 0;
    ctx->truncated = false;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_ctx_t *ctx = evt->user_data;
    if (!ctx || evt->event_id != HTTP_EVENT_ON_HEADER ||
        !evt->header_key || !evt->header_value) {
        return ESP_OK;
    }
    size_t key_len = strlen(evt->header_key);
    size_t val_len = strlen(evt->header_value);
    if (ctx->header_count >= HTTP_HEADER_MAX ||
        key_len >= HTTP_HEADER_KEY_MAX || val_len >= HTTP_HEADER_VAL_MAX ||
        ctx->header_bytes + key_len + val_len > HTTP_HEADER_BYTES) {
        ctx->truncated = true;
        return ESP_OK;
    }
    http_header_t *slot = &ctx->headers[ctx->header_count++];
    for (size_t i = 0; i <= key_len; i++) {
        char c = evt->header_key[i];
        slot->key[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    strlcpy(slot->value, evt->header_value, sizeof(slot->value));
    ctx->header_bytes += key_len + val_len;
    return ESP_OK;
}

static bool http_redirect_status(int status)
{
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

static bool http_prepare_timeout(lua_State *L, esp_http_client_handle_t client,
                                 int64_t deadline, bool full)
{
    int remaining = lua_net_remaining_ms(deadline);
    if (remaining == 0 || lua_net_stopped(L)) {
        return false;
    }
    /* open() performs DNS + TCP + TLS; a 100 ms socket timeout would abort the
     * handshake. Only the read/write loops use short slices. */
    int slice = remaining;
    if (!full && remaining > HTTP_IO_SLICE_MS) {
        slice = HTTP_IO_SLICE_MS;
    }
    (void)esp_http_client_set_timeout_ms(client, slice);
    return true;
}

static int push_http_error(lua_State *L, const char *reason)
{
    lua_pushnil(L);
    lua_pushstring(L, reason);
    return 2;
}

static void http_free_headers(lua_State *L, http_ctx_t *ctx, int table_index)
{
    for (size_t i = 0; i < ctx->header_count; i++) {
        lua_pushstring(L, ctx->headers[i].value);
        lua_setfield(L, table_index, ctx->headers[i].key);
    }
}

static int lua_net_http_request(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    lua_getfield(L, 1, "url");
    const char *url = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
    if (!url) {
        return luaL_error(L, "net.http.request: url is required");
    }
    lua_getfield(L, 1, "method");
    const char *method_str = lua_isstring(L, -1) ? lua_tostring(L, -1) : "GET";
    lua_getfield(L, 1, "body");
    const char *body = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
    size_t body_len = body ? strlen(body) : 0;
    lua_getfield(L, 1, "save_path");
    const char *save_path = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
    lua_getfield(L, 1, "ca_pem");
    const char *ca_pem = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;

    lua_getfield(L, 1, "timeout_ms");
    int timeout_ms = lua_isinteger(L, -1) ? (int)lua_tointeger(L, -1) : 10000;
    lua_pop(L, 1);
    lua_getfield(L, 1, "max_body");
    int max_body = lua_isinteger(L, -1) ? (int)lua_tointeger(L, -1) : HTTP_BODY_DEFAULT;
    lua_pop(L, 1);
    lua_getfield(L, 1, "follow_redirects");
    int follow = lua_isinteger(L, -1) ? (int)lua_tointeger(L, -1) : 3;
    lua_pop(L, 1);
    lua_getfield(L, 1, "insecure");
    bool insecure = lua_toboolean(L, -1);
    lua_pop(L, 1);

    if (timeout_ms <= 0) {
        timeout_ms = 10000;
    }
    if (max_body <= 0 || max_body > HTTP_BODY_MAX) {
        max_body = HTTP_BODY_DEFAULT;
    }
    if (follow < 0) {
        follow = 0;
    }
    if (follow > 10) {
        follow = 10;
    }

    esp_http_client_method_t method = HTTP_METHOD_GET;
    if (strcasecmp(method_str, "GET") == 0) {
        method = HTTP_METHOD_GET;
    } else if (strcasecmp(method_str, "POST") == 0) {
        method = HTTP_METHOD_POST;
    } else if (strcasecmp(method_str, "PUT") == 0) {
        method = HTTP_METHOD_PUT;
    } else if (strcasecmp(method_str, "PATCH") == 0) {
        method = HTTP_METHOD_PATCH;
    } else if (strcasecmp(method_str, "DELETE") == 0) {
        method = HTTP_METHOD_DELETE;
    } else if (strcasecmp(method_str, "HEAD") == 0) {
        method = HTTP_METHOD_HEAD;
    } else {
        return luaL_error(L, "unsupported method: %s", method_str);
    }

    int64_t deadline = lua_net_deadline(L, timeout_ms);
    http_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return luaL_error(L, "out of memory");
    }

    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .timeout_ms = timeout_ms,
        .buffer_size = HTTP_READ_CHUNK,
        .disable_auto_redirect = true,
        .keep_alive_enable = false,
        .event_handler = http_event_handler,
        .user_data = ctx,
    };
    if (insecure) {
        config.skip_cert_common_name_check = true;
    } else if (ca_pem) {
        config.cert_pem = ca_pem;
        config.cert_len = strlen(ca_pem) + 1;
    } else {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(ctx);
        return push_http_error(L, "client init failed");
    }

    /* Request headers from the Lua table (applied to every redirect hop). */
    lua_getfield(L, 1, "headers");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
                esp_http_client_set_header(client, lua_tostring(L, -2),
                                           lua_tostring(L, -1));
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    char *body_buf = NULL;
    size_t body_cap = 0;
    size_t body_used = 0;
    bool truncated = false;
    FILE *file = NULL;
    char tmp_path[256] = {0};
    if (save_path) {
        if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", save_path) >=
            (int)sizeof(tmp_path)) {
            esp_http_client_cleanup(client);
            free(ctx);
            return push_http_error(L, "save_path too long");
        }
        file = fopen(tmp_path, "wb");
        if (!file) {
            esp_http_client_cleanup(client);
            free(ctx);
            return push_http_error(L, "save_path open failed");
        }
    }

    char *chunk = malloc(HTTP_READ_CHUNK);
    if (!chunk) {
        if (file) {
            fclose(file);
            unlink(tmp_path);
        }
        esp_http_client_cleanup(client);
        free(ctx);
        return luaL_error(L, "out of memory");
    }

    const char *fail_reason = NULL;
    int status = 0;
    int redirects = 0;

    while (true) {
        http_ctx_reset(ctx);
        esp_err_t err;
        do {
            if (!http_prepare_timeout(L, client, deadline, true)) {
                fail_reason = lua_net_stopped(L) ? "stopped" : "timeout";
                goto done;
            }
            err = esp_http_client_open(client, (int)body_len);
            if (err == ESP_ERR_HTTP_EAGAIN) {
                vTaskDelay(1);
            }
        } while (err == ESP_ERR_HTTP_EAGAIN);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "open failed: %s", esp_err_to_name(err));
            fail_reason = "connect failed";
            goto done;
        }

        size_t sent = 0;
        while (sent < body_len) {
            if (!http_prepare_timeout(L, client, deadline, false)) {
                fail_reason = lua_net_stopped(L) ? "stopped" : "timeout";
                goto done;
            }
            int written = esp_http_client_write(client, body + sent,
                                                (int)(body_len - sent));
            if (written > 0) {
                sent += (size_t)written;
            } else if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                fail_reason = "write failed";
                goto done;
            } else {
                vTaskDelay(1);
            }
        }

        int64_t content_len = -1;
        while (true) {
            if (!http_prepare_timeout(L, client, deadline, false)) {
                fail_reason = lua_net_stopped(L) ? "stopped" : "timeout";
                goto done;
            }
            content_len = esp_http_client_fetch_headers(client);
            if (content_len >= 0) {
                break;
            }
            if (content_len != -ESP_ERR_HTTP_EAGAIN) {
                fail_reason = "invalid response";
                goto done;
            }
            vTaskDelay(1);
        }

        while (true) {
            if (!http_prepare_timeout(L, client, deadline, false)) {
                fail_reason = lua_net_stopped(L) ? "stopped" : "timeout";
                goto done;
            }
            int got = esp_http_client_read(client, chunk, HTTP_READ_CHUNK);
            if (got == -ESP_ERR_HTTP_EAGAIN) {
                vTaskDelay(1);
                continue;
            }
            if (got < 0) {
                fail_reason = "read failed";
                goto done;
            }
            if (got == 0) {
                if (!esp_http_client_is_complete_data_received(client)) {
                    fail_reason = "incomplete response";
                    goto done;
                }
                break;
            }
            if (file) {
                if (fwrite(chunk, 1, (size_t)got, file) != (size_t)got) {
                    fail_reason = "file write failed";
                    goto done;
                }
            } else {
                size_t want = (size_t)got;
                if (body_used + want > (size_t)max_body) {
                    want = (size_t)max_body - body_used;
                    truncated = true;
                }
                if (want > 0) {
                    if (body_used + want > body_cap) {
                        size_t cap = body_cap ? body_cap : 8192;
                        while (cap < body_used + want) {
                            cap *= 2;
                        }
                        char *grown = realloc(body_buf, cap);
                        if (!grown) {
                            fail_reason = "out of memory";
                            goto done;
                        }
                        body_buf = grown;
                        body_cap = cap;
                    }
                    memcpy(body_buf + body_used, chunk, want);
                    body_used += want;
                }
            }
        }

        status = esp_http_client_get_status_code(client);
        if (http_redirect_status(status) && redirects < follow) {
            if (esp_http_client_set_redirection(client) != ESP_OK) {
                fail_reason = "redirect failed";
                goto done;
            }
            esp_http_client_close(client);
            redirects++;
            continue;
        }
        break;
    }

done:
    free(chunk);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (file) {
        bool close_ok = fclose(file) == 0;
        if (fail_reason || !close_ok) {
            unlink(tmp_path);
            free(body_buf);
            free(ctx);
            return push_http_error(L, fail_reason ? fail_reason : "file write failed");
        }
        if (rename(tmp_path, save_path) != 0) {
            unlink(tmp_path);
            free(body_buf);
            free(ctx);
            return push_http_error(L, "file rename failed");
        }
    }
    if (fail_reason) {
        free(body_buf);
        free(ctx);
        return push_http_error(L, fail_reason);
    }

    lua_newtable(L);
    lua_pushinteger(L, status);
    lua_setfield(L, -2, "status");
    lua_newtable(L);
    int headers_index = lua_gettop(L);
    http_free_headers(L, ctx, headers_index);
    lua_setfield(L, -2, "headers");
    if (file) {
        lua_pushstring(L, save_path);
        lua_setfield(L, -2, "saved");
    } else {
        lua_pushlstring(L, body_buf ? body_buf : "", body_used);
        lua_setfield(L, -2, "body");
        lua_pushboolean(L, truncated);
        lua_setfield(L, -2, "truncated");
    }
    free(body_buf);
    free(ctx);
    return 1;
}

int lua_net_http_module(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, lua_net_http_request);
    lua_setfield(L, -2, "request");
    return 1;
}
