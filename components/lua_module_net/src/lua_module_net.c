/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * `net` module: net.tcp / net.udp / net.http / net.resolve / net.online.
 */
#include "lua_module_net.h"

#include <string.h>

#include "cap_lua.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/inet.h"

static const char *TAG = "lua_module_net";

static esp_netif_t *net_if_with_ip(const char *ifkey, char *ip_out, size_t ip_size,
                                   char *netmask_out, size_t netmask_size)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(ifkey);
    if (!netif) {
        return NULL;
    }
    esp_netif_ip_info_t info = {0};
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK ||
        info.ip.addr == 0) {
        return NULL;
    }
    if (ip_out && !esp_ip4addr_ntoa(&info.ip, ip_out, ip_size)) {
        return NULL;
    }
    if (netmask_out) {
        esp_ip4addr_ntoa(&info.netmask, netmask_out, netmask_size);
    }
    return netif;
}

static bool net_is_online(char *ip_out, size_t ip_size)
{
    char netmask[16];
    if (net_if_with_ip("WIFI_STA_DEF", ip_out, ip_size, netmask, sizeof(netmask))) {
        return true;
    }
    if (net_if_with_ip("ETH_DEF", ip_out, ip_size, netmask, sizeof(netmask))) {
        return true;
    }
    return false;
}

static int lua_net_online(lua_State *L)
{
    char ip[16];
    lua_pushboolean(L, net_is_online(ip, sizeof(ip)));
    return 1;
}

static int lua_net_local_ip(lua_State *L)
{
    char ip[16] = {0};
    if (!net_is_online(ip, sizeof(ip))) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, ip);
    return 1;
}

void lua_net_netif_register(lua_State *L)
{
    lua_pushcfunction(L, lua_net_online);
    lua_setfield(L, -2, "online");
    lua_pushcfunction(L, lua_net_local_ip);
    lua_setfield(L, -2, "local_ip");
}

int luaopen_net(lua_State *L)
{
    lua_newtable(L);

    lua_net_netif_register(L);
    lua_net_dns_register(L);

    lua_net_tcp_module(L);
    lua_setfield(L, -2, "tcp");
    lua_net_udp_module(L);
    lua_setfield(L, -2, "udp");
    lua_net_http_module(L);
    lua_setfield(L, -2, "http");

    return 1;
}

esp_err_t lua_module_net_register(void)
{
    esp_err_t err = cap_lua_register_module("net", luaopen_net);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Lua net module registered");
    }
    return err;
}
