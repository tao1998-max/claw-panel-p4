/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * net.resolve: lwip DNS with an abortable wait. lwip's public API exposes one
 * address per lookup (DNS_MAX_HOST_IP only controls the cache), so callers
 * that need both families call resolve twice.
 */
#include "lua_module_net.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/dns.h"
#include "lwip/inet.h"
#include "lwip/ip_addr.h"

static const char *TAG = "lua_net_dns";

typedef struct {
    SemaphoreHandle_t done;
    ip_addr_t addr;
    err_t err;
} lua_net_dns_ctx_t;

static void lua_net_dns_cb(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    (void)name;
    lua_net_dns_ctx_t *ctx = arg;
    if (ipaddr) {
        ctx->addr = *ipaddr;
        ctx->err = ERR_OK;
    } else {
        ctx->err = ERR_VAL;
    }
    xSemaphoreGive(ctx->done);
}

bool lua_net_resolve_sync(lua_State *L, const char *host, int family, int timeout_ms,
                          char *out_ip, size_t out_size, const char **err)
{
    if (!host || !host[0]) {
        *err = "invalid host";
        return false;
    }

    /* Numeric addresses need no lookup. */
    ip_addr_t literal;
    if (ipaddr_aton(host, &literal)) {
        if (!ipaddr_ntoa_r(&literal, out_ip, out_size)) {
            *err = "address too long";
            return false;
        }
        return true;
    }

    lua_net_dns_ctx_t ctx = {0};
    ctx.done = xSemaphoreCreateBinary();
    if (!ctx.done) {
        *err = "out of memory";
        return false;
    }

    ip_addr_t addr;
    memset(&addr, 0, sizeof(addr));
    err_t start = dns_gethostbyname_addrtype(host, &addr, lua_net_dns_cb, &ctx,
                                             (u8_t)family);
    if (start == ERR_OK) {
        vSemaphoreDelete(ctx.done);
        if (!ipaddr_ntoa_r(&addr, out_ip, out_size)) {
            *err = "address too long";
            return false;
        }
        return true;
    }
    if (start != ERR_INPROGRESS) {
        vSemaphoreDelete(ctx.done);
        ESP_LOGW(TAG, "dns start failed for %s: %d", host, start);
        *err = "dns failed";
        return false;
    }

    int64_t deadline = lua_net_deadline(L, timeout_ms > 0 ? timeout_ms : 3000);
    bool ok = false;
    while (true) {
        if (lua_net_stopped(L)) {
            *err = "stopped";
            break;
        }
        int remaining = lua_net_remaining_ms(deadline);
        if (remaining == 0) {
            *err = "dns timeout";
            break;
        }
        if (xSemaphoreTake(ctx.done, pdMS_TO_TICKS(remaining > 50 ? 50 : remaining)) == pdTRUE) {
            if (ctx.err == ERR_OK && ipaddr_ntoa_r(&ctx.addr, out_ip, out_size)) {
                ok = true;
            } else {
                *err = "dns failed";
            }
            break;
        }
    }
    vSemaphoreDelete(ctx.done);
    return ok;
}

static int net_resolve(lua_State *L)
{
    const char *host = luaL_checkstring(L, 1);
    int timeout = 3000;
    int family = LWIP_DNS_ADDRTYPE_IPV4;
    const char *family_str = NULL;

    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "timeout_ms");
        if (lua_isinteger(L, -1)) {
            timeout = (int)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "family");
        if (lua_isstring(L, -1)) {
            family_str = lua_tostring(L, -1);
        }
        if (family_str && (strcmp(family_str, "AAAA") == 0 ||
                           strcmp(family_str, "ipv6") == 0 ||
                           strcmp(family_str, "IPv6") == 0)) {
            family = LWIP_DNS_ADDRTYPE_IPV6;
        }
    }

    char ip[48] = {0};
    const char *err = NULL;
    if (!lua_net_resolve_sync(L, host, family, timeout, ip, sizeof(ip), &err)) {
        lua_pushnil(L);
        lua_pushstring(L, err ? err : "dns failed");
        return 2;
    }
    lua_pushstring(L, ip);
    return 1;
}

void lua_net_dns_register(lua_State *L)
{
    lua_pushcfunction(L, net_resolve);
    lua_setfield(L, -2, "resolve");
}
