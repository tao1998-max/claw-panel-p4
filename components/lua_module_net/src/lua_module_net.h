/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internals for the `net` Lua module (TCP / UDP / DNS / HTTP).
 * Budget and wait helpers live in lua_net_common and are shared with the
 * mqtt and websocket modules.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_tls.h"
#include "lauxlib.h"
#include "lua.h"
#include "lua_net_common.h"

#define LUA_NET_RECV_MAX  (64 * 1024)
#define LUA_NET_LINE_MAX  1024
#define LUA_NET_HOST_MAX  128
#define LUA_NET_URL_MAX   512

/* Blocking TLS handshake executed on an internal-RAM worker task (the Lua job
 * stack is only 12 KiB). The connection is left in non-blocking mode, so
 * later reads return ESP_TLS_ERR_SSL_WANT_READ instead of blocking. */
esp_tls_t *lua_net_tls_connect(lua_State *L, const char *host, int port,
                               bool insecure, const char *ca_pem,
                               const char *server_name, int timeout_ms,
                               const char **err);
/* >0 bytes, 0 = closed, -1 = stopped, -2 = error, -3 = timeout. */
int lua_net_tls_read(lua_State *L, esp_tls_t *tls, char *buf, size_t len,
                     int64_t deadline);
/* >0 bytes written, -1 = stopped, -2 = error, -3 = timeout. */
int lua_net_tls_write(lua_State *L, esp_tls_t *tls, const char *buf, size_t len,
                      int64_t deadline);
void lua_net_tls_close(esp_tls_t *tls);

/* DNS via lwip (single address). family = LWIP_DNS_ADDRTYPE_IPV4/_IPV6. */
bool lua_net_resolve_sync(lua_State *L, const char *host, int family, int timeout_ms,
                          char *out_ip, size_t out_size, const char **err);

typedef struct {
    bool tls;
    char host[LUA_NET_HOST_MAX];
    int port;
    char path[LUA_NET_URL_MAX];
} lua_net_url_t;

bool lua_net_parse_url(const char *url, lua_net_url_t *out);

/* Sub-module constructors; each pushes one table. */
int lua_net_tcp_module(lua_State *L);
int lua_net_udp_module(lua_State *L);
int lua_net_http_module(lua_State *L);
void lua_net_dns_register(lua_State *L);
void lua_net_netif_register(lua_State *L);

/* Registration entry (called from claw_port_init). */
esp_err_t lua_module_net_register(void);
