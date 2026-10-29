/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * net.udp: datagram sockets with an optional fixed bind port, broadcast,
 * TTL and multicast join.
 */
#include "lua_module_net.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "lwip/dns.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "lua_net_udp";

#define UDP_MT "net.udp"

typedef struct {
    int fd;
    bool open;
    bool budget_socket;
    int default_timeout_ms;
} lua_net_udp_ud_t;

static int udp_timeout_opt(lua_State *L, int index, int fallback)
{
    if (!lua_istable(L, index)) {
        return fallback;
    }
    lua_getfield(L, index, "timeout_ms");
    int timeout = lua_isinteger(L, -1) ? (int)lua_tointeger(L, -1) : fallback;
    lua_pop(L, 1);
    return timeout > 0 ? timeout : fallback;
}

static void udp_release(lua_State *L, lua_net_udp_ud_t *ud)
{
    if (!ud || !ud->open) {
        return;
    }
    ud->open = false;
    if (ud->fd >= 0) {
        close(ud->fd);
        ud->fd = -1;
    }
    if (ud->budget_socket) {
        lua_net_budget_release(L, LUA_NET_KIND_SOCKET);
        ud->budget_socket = false;
    }
}

static lua_net_udp_ud_t *udp_check(lua_State *L, int index)
{
    lua_net_udp_ud_t *ud = luaL_checkudata(L, index, UDP_MT);
    if (!ud || !ud->open) {
        luaL_error(L, "net.udp socket is closed");
        return NULL;
    }
    return ud;
}

static bool udp_resolve(lua_State *L, const char *host, int port,
                        struct sockaddr_storage *out, socklen_t *out_len,
                        int timeout_ms, const char **err)
{
    memset(out, 0, sizeof(*out));
    struct sockaddr_in *v4 = (struct sockaddr_in *)out;
    if (inet_pton(AF_INET, host, &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons((uint16_t)port);
        *out_len = sizeof(*v4);
        return true;
    }
    struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)out;
    if (inet_pton(AF_INET6, host, &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons((uint16_t)port);
        *out_len = sizeof(*v6);
        return true;
    }
    char ip[48];
    if (!lua_net_resolve_sync(L, host, LWIP_DNS_ADDRTYPE_IPV4, timeout_ms,
                              ip, sizeof(ip), err)) {
        return false;
    }
    if (inet_pton(AF_INET, ip, &v4->sin_addr) != 1) {
        *err = "dns returned an unusable address";
        return false;
    }
    v4->sin_family = AF_INET;
    v4->sin_port = htons((uint16_t)port);
    *out_len = sizeof(*v4);
    return true;
}

static int udp_new(lua_State *L)
{
    int port = 0;
    bool reuse = true;
    bool broadcast = false;
    int ttl = 0;
    int timeout = LUA_NET_DEF_TIMEOUT_MS;

    if (!lua_isnoneornil(L, 1)) {
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_getfield(L, 1, "port");
        if (lua_isinteger(L, -1)) {
            port = (int)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);
        lua_getfield(L, 1, "reuse");
        if (lua_isboolean(L, -1)) {
            reuse = lua_toboolean(L, -1);
        }
        lua_pop(L, 1);
        lua_getfield(L, 1, "broadcast");
        if (lua_isboolean(L, -1)) {
            broadcast = lua_toboolean(L, -1);
        }
        lua_pop(L, 1);
        lua_getfield(L, 1, "ttl");
        if (lua_isinteger(L, -1)) {
            ttl = (int)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);
        timeout = udp_timeout_opt(L, 1, timeout);
    }
    if (port < 0 || port > 65535) {
        return luaL_error(L, "port must be 0..65535");
    }

    const char *err = NULL;
    if (!lua_net_budget_acquire(L, LUA_NET_KIND_SOCKET, &err)) {
        lua_pushnil(L);
        lua_pushstring(L, err);
        return 2;
    }

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        lua_net_budget_release(L, LUA_NET_KIND_SOCKET);
        lua_pushnil(L);
        lua_pushliteral(L, "socket failed");
        return 2;
    }
    if (reuse) {
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    }
    if (broadcast) {
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    }
    if (ttl > 0) {
        int value = ttl;
        setsockopt(fd, IPPROTO_IP, IP_TTL, &value, sizeof(value));
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons((uint16_t)port),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        lua_net_budget_release(L, LUA_NET_KIND_SOCKET);
        lua_pushnil(L);
        lua_pushliteral(L, "bind failed");
        return 2;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(fd);
        lua_net_budget_release(L, LUA_NET_KIND_SOCKET);
        lua_pushnil(L);
        lua_pushliteral(L, "fcntl failed");
        return 2;
    }

    lua_net_udp_ud_t *ud = lua_newuserdata(L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    ud->fd = fd;
    ud->open = true;
    ud->budget_socket = true;
    ud->default_timeout_ms = timeout;
    luaL_setmetatable(L, UDP_MT);
    return 1;
}

static int udp_send(lua_State *L)
{
    lua_net_udp_ud_t *ud = udp_check(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    const char *host = luaL_checkstring(L, 3);
    int port = (int)luaL_checkinteger(L, 4);
    if (port <= 0 || port > 65535) {
        return luaL_error(L, "port must be 1..65535");
    }
    int timeout = udp_timeout_opt(L, 5, ud->default_timeout_ms);
    int64_t deadline = lua_net_deadline(L, timeout);

    struct sockaddr_storage addr;
    socklen_t addr_len = 0;
    const char *err = NULL;
    if (!udp_resolve(L, host, port, &addr, &addr_len, timeout, &err)) {
        lua_pushnil(L);
        lua_pushstring(L, err ? err : "dns failed");
        return 2;
    }

    while (true) {
        int ready = lua_net_wait_fd(L, ud->fd, true, deadline);
        if (ready <= 0) {
            lua_pushnil(L);
            lua_pushstring(L, ready == 0 ? "timeout" : (ready == -1 ? "stopped" : "io error"));
            return 2;
        }
        int sent = sendto(ud->fd, data, len, 0, (struct sockaddr *)&addr, addr_len);
        if (sent >= 0) {
            lua_pushinteger(L, sent);
            return 1;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            continue;
        }
        lua_pushnil(L);
        lua_pushliteral(L, "sendto failed");
        return 2;
    }
}

static int udp_recv(lua_State *L)
{
    lua_net_udp_ud_t *ud = udp_check(L, 1);
    lua_Integer want = luaL_optinteger(L, 2, 0);
    if (want <= 0 || want > LUA_NET_RECV_MAX) {
        want = LUA_NET_RECV_MAX;
    }
    int timeout = udp_timeout_opt(L, 3, ud->default_timeout_ms);
    int64_t deadline = lua_net_deadline(L, timeout);

    char *buf = malloc((size_t)want);
    if (!buf) {
        return luaL_error(L, "out of memory");
    }

    while (true) {
        int ready = lua_net_wait_fd(L, ud->fd, false, deadline);
        if (ready <= 0) {
            free(buf);
            lua_pushnil(L);
            lua_pushstring(L, ready == 0 ? "timeout" : (ready == -1 ? "stopped" : "io error"));
            return 2;
        }
        struct sockaddr_storage from;
        socklen_t from_len = sizeof(from);
        int got = recvfrom(ud->fd, buf, (size_t)want, 0,
                           (struct sockaddr *)&from, &from_len);
        if (got >= 0) {
            char ip[48] = {0};
            int port = 0;
            if (from.ss_family == AF_INET) {
                struct sockaddr_in *v4 = (struct sockaddr_in *)&from;
                inet_ntop(AF_INET, &v4->sin_addr, ip, sizeof(ip));
                port = ntohs(v4->sin_port);
            } else if (from.ss_family == AF_INET6) {
                struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&from;
                inet_ntop(AF_INET6, &v6->sin6_addr, ip, sizeof(ip));
                port = ntohs(v6->sin6_port);
            }
            lua_pushlstring(L, buf, (size_t)got);
            lua_pushstring(L, ip);
            lua_pushinteger(L, port);
            free(buf);
            return 3;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            continue;
        }
        ESP_LOGW(TAG, "recvfrom failed: errno=%d", errno);
        free(buf);
        lua_pushnil(L);
        lua_pushliteral(L, "recv failed");
        return 2;
    }
}

static int udp_set_broadcast(lua_State *L)
{
    lua_net_udp_ud_t *ud = udp_check(L, 1);
    int on = lua_toboolean(L, 2);
    if (setsockopt(ud->fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) != 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "setsockopt failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int udp_set_ttl(lua_State *L)
{
    lua_net_udp_ud_t *ud = udp_check(L, 1);
    int ttl = (int)luaL_checkinteger(L, 2);
    if (setsockopt(ud->fd, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) != 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "setsockopt failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int udp_join_multicast(lua_State *L)
{
    lua_net_udp_ud_t *ud = udp_check(L, 1);
    const char *group = luaL_checkstring(L, 2);
    struct ip_mreq mreq = {0};
    if (inet_pton(AF_INET, group, &mreq.imr_multiaddr.s_addr) != 1) {
        return luaL_error(L, "multicast group must be an IPv4 address");
    }
    const char *if_ip = "0.0.0.0";
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "if_ip");
        if (lua_isstring(L, -1)) {
            if_ip = lua_tostring(L, -1);
        }
        lua_pushvalue(L, -1); /* keep the string alive below */
    } else {
        lua_pushnil(L);
    }
    if (inet_pton(AF_INET, if_ip, &mreq.imr_interface.s_addr) != 1) {
        lua_pop(L, 1);
        return luaL_error(L, "if_ip must be an IPv4 address");
    }
    int ret = setsockopt(ud->fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    lua_pop(L, 1);
    if (ret != 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "join multicast failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int udp_local(lua_State *L)
{
    lua_net_udp_ud_t *ud = udp_check(L, 1);
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (getsockname(ud->fd, (struct sockaddr *)&addr, &len) != 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "getsockname failed");
        return 2;
    }
    char ip[48] = {0};
    int port = 0;
    if (addr.ss_family == AF_INET) {
        struct sockaddr_in *v4 = (struct sockaddr_in *)&addr;
        inet_ntop(AF_INET, &v4->sin_addr, ip, sizeof(ip));
        port = ntohs(v4->sin_port);
    } else if (addr.ss_family == AF_INET6) {
        struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&addr;
        inet_ntop(AF_INET6, &v6->sin6_addr, ip, sizeof(ip));
        port = ntohs(v6->sin6_port);
    }
    lua_newtable(L);
    lua_pushstring(L, ip);
    lua_setfield(L, -2, "ip");
    lua_pushinteger(L, port);
    lua_setfield(L, -2, "port");
    return 1;
}

static int udp_close(lua_State *L)
{
    lua_net_udp_ud_t *ud = luaL_checkudata(L, 1, UDP_MT);
    udp_release(L, ud);
    return 0;
}

static int udp_gc(lua_State *L)
{
    lua_net_udp_ud_t *ud = luaL_testudata(L, 1, UDP_MT);
    if (ud) {
        udp_release(L, ud);
    }
    return 0;
}

static const luaL_Reg udp_methods[] = {
    {"send", udp_send},
    {"recv", udp_recv},
    {"set_broadcast", udp_set_broadcast},
    {"set_ttl", udp_set_ttl},
    {"join_multicast", udp_join_multicast},
    {"local_addr", udp_local},
    {"close", udp_close},
    {NULL, NULL},
};

int lua_net_udp_module(lua_State *L)
{
    if (luaL_newmetatable(L, UDP_MT)) {
        lua_pushcfunction(L, udp_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushcfunction(L, udp_close);
        lua_setfield(L, -2, "__close");
        lua_newtable(L);
        luaL_setfuncs(L, udp_methods, 0);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, udp_new);
    lua_setfield(L, -2, "new");
    return 1;
}
