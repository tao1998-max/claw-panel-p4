/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * net.tcp: blocking sockets with deadlines, optional TLS, plus a small
 * listen/accept pair (used for LAN tests; not a high-performance server).
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

static const char *TAG = "lua_net_tcp";

#define TCP_CONN_MT "net.tcp.conn"
#define TCP_SRV_MT  "net.tcp.server"

typedef struct {
    int fd;
    esp_tls_t *tls;
    bool open;
    bool is_tls;
    bool budget_socket;
    bool budget_tls;
    int default_timeout_ms;
    char pending[LUA_NET_LINE_MAX];
    size_t pending_len;
} lua_net_tcp_ud_t;

typedef struct {
    int fd;
    bool open;
    bool budget_socket;
    int default_timeout_ms;
} lua_net_tcp_srv_ud_t;

static int tcp_timeout_opt(lua_State *L, int index, int fallback)
{
    if (!lua_istable(L, index)) {
        return fallback;
    }
    lua_getfield(L, index, "timeout_ms");
    int timeout = lua_isinteger(L, -1) ? (int)lua_tointeger(L, -1) : fallback;
    lua_pop(L, 1);
    return timeout > 0 ? timeout : fallback;
}

static bool tcp_opt_bool(lua_State *L, int index, const char *key)
{
    if (!lua_istable(L, index)) {
        return false;
    }
    lua_getfield(L, index, key);
    bool value = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return value;
}

/* Resolve host to a sockaddr. Returns true on success (IPv4 or IPv6 literal). */
static bool tcp_resolve_addr(lua_State *L, const char *host, int port,
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

static bool tcp_set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void tcp_release(lua_State *L, lua_net_tcp_ud_t *ud)
{
    if (!ud || !ud->open) {
        return;
    }
    ud->open = false;
    if (ud->tls) {
        lua_net_tls_close(ud->tls);
        ud->tls = NULL;
    }
    if (ud->fd >= 0) {
        close(ud->fd);
        ud->fd = -1;
    }
    if (ud->budget_tls) {
        lua_net_budget_release(L, LUA_NET_KIND_TLS);
        ud->budget_tls = false;
    }
    if (ud->budget_socket) {
        lua_net_budget_release(L, LUA_NET_KIND_SOCKET);
        ud->budget_socket = false;
    }
}

static lua_net_tcp_ud_t *tcp_check(lua_State *L, int index)
{
    lua_net_tcp_ud_t *ud = luaL_checkudata(L, index, TCP_CONN_MT);
    if (!ud || !ud->open) {
        luaL_error(L, "net.tcp connection is closed");
        return NULL;
    }
    return ud;
}

static lua_net_tcp_srv_ud_t *tcp_srv_check(lua_State *L, int index)
{
    lua_net_tcp_srv_ud_t *ud = luaL_checkudata(L, index, TCP_SRV_MT);
    if (!ud || !ud->open) {
        luaL_error(L, "net.tcp listener is closed");
        return NULL;
    }
    return ud;
}

/* Read once. >0 bytes, 0 timeout, -1 stopped, -2 error, -3 closed. */
static int tcp_read_once(lua_State *L, lua_net_tcp_ud_t *ud, char *buf, size_t len,
                         int64_t deadline)
{
    if (ud->is_tls) {
        /* lua_net_tls_read reads first (mbedTLS may already hold plaintext)
         * and only then waits on the socket. Map its codes to ours. */
        int got = lua_net_tls_read(L, ud->tls, buf, len, deadline);
        if (got > 0) {
            return got;
        }
        if (got == 0) {
            return -3; /* peer closed */
        }
        if (got == -3) {
            return 0; /* timeout */
        }
        return got == -1 ? -1 : -2;
    }

    while (true) {
        int ready = lua_net_wait_fd(L, ud->fd, false, deadline);
        if (ready <= 0) {
            return ready; /* 0 timeout, -1 stopped, -2 select error */
        }
        int got = recv(ud->fd, buf, len, 0);
        if (got > 0) {
            return got;
        }
        if (got == 0) {
            return -3;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            continue;
        }
        ESP_LOGW(TAG, "recv failed: errno=%d", errno);
        return -2;
    }
}

static int push_read_result(lua_State *L, int result, const char *data, size_t len)
{
    switch (result) {
    case -1:
        lua_pushnil(L);
        lua_pushliteral(L, "stopped");
        return 2;
    case -2:
        lua_pushnil(L);
        lua_pushliteral(L, "io error");
        return 2;
    case -3:
        lua_pushnil(L);
        lua_pushliteral(L, "closed");
        return 2;
    case 0:
        lua_pushnil(L);
        lua_pushliteral(L, "timeout");
        return 2;
    default:
        lua_pushlstring(L, data, len);
        return 1;
    }
}

static int tcp_send_impl(lua_State *L, bool all)
{
    lua_net_tcp_ud_t *ud = tcp_check(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    int timeout = tcp_timeout_opt(L, 3, ud->default_timeout_ms);
    int64_t deadline = lua_net_deadline(L, timeout);
    size_t sent = 0;

    while (sent < len) {
        if (lua_net_stopped(L)) {
            lua_pushnil(L);
            lua_pushliteral(L, "stopped");
            lua_pushinteger(L, (lua_Integer)sent);
            return 3;
        }
        if (lua_net_remaining_ms(deadline) == 0) {
            if (!all && sent > 0) {
                lua_pushinteger(L, (lua_Integer)sent);
                return 1;
            }
            lua_pushnil(L);
            lua_pushliteral(L, "timeout");
            lua_pushinteger(L, (lua_Integer)sent);
            return 3;
        }
        int wrote;
        if (ud->is_tls) {
            wrote = lua_net_tls_write(L, ud->tls, data + sent, len - sent, deadline);
            if (wrote > 0) {
                sent += (size_t)wrote;
                if (!all) {
                    break;
                }
                continue;
            }
            if (wrote == -3) {
                if (!all && sent > 0) {
                    lua_pushinteger(L, (lua_Integer)sent);
                    return 1;
                }
                lua_pushnil(L);
                lua_pushliteral(L, "timeout");
                lua_pushinteger(L, (lua_Integer)sent);
                return 3;
            }
            lua_pushnil(L);
            lua_pushstring(L, wrote == -1 ? "stopped" : "tls write error");
            lua_pushinteger(L, (lua_Integer)sent);
            return 3;
        }
        int ready = lua_net_wait_fd(L, ud->fd, true, deadline);
        if (ready <= 0) {
            if (!all && sent > 0) {
                lua_pushinteger(L, (lua_Integer)sent);
                return 1;
            }
            lua_pushnil(L);
            lua_pushstring(L, ready == 0 ? "timeout" : "stopped");
            lua_pushinteger(L, (lua_Integer)sent);
            return 3;
        }
        wrote = send(ud->fd, data + sent, len - sent, 0);
        if (wrote > 0) {
            sent += (size_t)wrote;
            if (!all) {
                break;
            }
            continue;
        }
        if (wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            continue;
        }
        lua_pushnil(L);
        lua_pushliteral(L, "io error");
        lua_pushinteger(L, (lua_Integer)sent);
        return 3;
    }
    lua_pushinteger(L, (lua_Integer)sent);
    return 1;
}

static int tcp_send(lua_State *L)
{
    return tcp_send_impl(L, false);
}

static int tcp_send_all(lua_State *L)
{
    int pushed = tcp_send_impl(L, true);
    if (pushed == 1 && lua_isinteger(L, -1)) {
        lua_pushboolean(L, 1);
        return 1;
    }
    return pushed;
}

static int tcp_recv(lua_State *L)
{
    lua_net_tcp_ud_t *ud = tcp_check(L, 1);
    lua_Integer want = luaL_optinteger(L, 2, 0);
    if (want <= 0 || want > LUA_NET_RECV_MAX) {
        want = LUA_NET_RECV_MAX;
    }
    int timeout = tcp_timeout_opt(L, 3, ud->default_timeout_ms);
    int64_t deadline = lua_net_deadline(L, timeout);

    if (ud->pending_len > 0) {
        size_t take = (size_t)want < ud->pending_len ? (size_t)want : ud->pending_len;
        lua_pushlstring(L, ud->pending, take);
        memmove(ud->pending, ud->pending + take, ud->pending_len - take);
        ud->pending_len -= take;
        return 1;
    }

    char *buf = malloc((size_t)want);
    if (!buf) {
        return luaL_error(L, "out of memory");
    }
    int got = tcp_read_once(L, ud, buf, (size_t)want, deadline);
    int pushed = push_read_result(L, got, buf, got > 0 ? (size_t)got : 0);
    free(buf);
    return pushed;
}

static int tcp_recv_exact(lua_State *L)
{
    lua_net_tcp_ud_t *ud = tcp_check(L, 1);
    lua_Integer want = luaL_checkinteger(L, 2);
    if (want <= 0 || want > LUA_NET_RECV_MAX) {
        return luaL_error(L, "length must be 1..%d", LUA_NET_RECV_MAX);
    }
    int timeout = tcp_timeout_opt(L, 3, ud->default_timeout_ms);
    int64_t deadline = lua_net_deadline(L, timeout);

    char *buf = malloc((size_t)want);
    if (!buf) {
        return luaL_error(L, "out of memory");
    }
    size_t got_total = 0;
    if (ud->pending_len > 0) {
        size_t take = (size_t)want < ud->pending_len ? (size_t)want : ud->pending_len;
        memcpy(buf, ud->pending, take);
        memmove(ud->pending, ud->pending + take, ud->pending_len - take);
        ud->pending_len -= take;
        got_total = take;
    }
    while (got_total < (size_t)want) {
        int got = tcp_read_once(L, ud, buf + got_total, (size_t)want - got_total, deadline);
        if (got > 0) {
            got_total += (size_t)got;
            continue;
        }
        int pushed = push_read_result(L, got, NULL, 0);
        free(buf);
        return pushed;
    }
    lua_pushlstring(L, buf, got_total);
    free(buf);
    return 1;
}

static int tcp_recv_line(lua_State *L)
{
    lua_net_tcp_ud_t *ud = tcp_check(L, 1);
    int timeout = tcp_timeout_opt(L, 2, ud->default_timeout_ms);
    int64_t deadline = lua_net_deadline(L, timeout);

    while (true) {
        char *nl = memchr(ud->pending, '\n', ud->pending_len);
        if (nl) {
            size_t line_len = (size_t)(nl - ud->pending) + 1;
            lua_pushlstring(L, ud->pending, line_len);
            memmove(ud->pending, ud->pending + line_len, ud->pending_len - line_len);
            ud->pending_len -= line_len;
            return 1;
        }
        if (ud->pending_len >= sizeof(ud->pending)) {
            lua_pushnil(L);
            lua_pushliteral(L, "line too long");
            return 2;
        }
        int got = tcp_read_once(L, ud, ud->pending + ud->pending_len,
                                sizeof(ud->pending) - ud->pending_len, deadline);
        if (got > 0) {
            ud->pending_len += (size_t)got;
            continue;
        }
        int pushed = push_read_result(L, got, NULL, 0);
        return pushed;
    }
}

static int tcp_wait(lua_State *L, bool for_write)
{
    lua_net_tcp_ud_t *ud = tcp_check(L, 1);
    int timeout = tcp_timeout_opt(L, 2, ud->default_timeout_ms);
    int ready = lua_net_wait_fd(L, ud->fd, for_write, lua_net_deadline(L, timeout));
    if (ready < 0) {
        lua_pushnil(L);
        lua_pushstring(L, ready == -1 ? "stopped" : "io error");
        return 2;
    }
    lua_pushboolean(L, ready == 1);
    return 1;
}

static int tcp_wait_readable(lua_State *L)
{
    return tcp_wait(L, false);
}

static int tcp_wait_writable(lua_State *L)
{
    return tcp_wait(L, true);
}

static int tcp_set_timeout(lua_State *L)
{
    lua_net_tcp_ud_t *ud = tcp_check(L, 1);
    int timeout = (int)luaL_checkinteger(L, 2);
    ud->default_timeout_ms = timeout > 0 ? timeout : LUA_NET_DEF_TIMEOUT_MS;
    return 0;
}

static int tcp_set_nodelay(lua_State *L)
{
    lua_net_tcp_ud_t *ud = tcp_check(L, 1);
    int on = lua_toboolean(L, 2);
    if (ud->is_tls) {
        lua_pushnil(L);
        lua_pushliteral(L, "not supported for TLS connections");
        return 2;
    }
    if (setsockopt(ud->fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) != 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "setsockopt failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int tcp_set_keepalive(lua_State *L)
{
    lua_net_tcp_ud_t *ud = tcp_check(L, 1);
    int on = 1;
    if (ud->is_tls) {
        lua_pushnil(L);
        lua_pushliteral(L, "not supported for TLS connections");
        return 2;
    }
    if (setsockopt(ud->fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) != 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "setsockopt failed");
        return 2;
    }
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "idle_s");
        if (lua_isinteger(L, -1)) {
            int idle = (int)lua_tointeger(L, -1);
            setsockopt(ud->fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "interval_s");
        if (lua_isinteger(L, -1)) {
            int interval = (int)lua_tointeger(L, -1);
            setsockopt(ud->fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
        }
        lua_pop(L, 1);
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int push_sockaddr(lua_State *L, const struct sockaddr_storage *addr)
{
    char ip[48] = {0};
    int port = 0;
    if (addr->ss_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)addr;
        inet_ntop(AF_INET, &v4->sin_addr, ip, sizeof(ip));
        port = ntohs(v4->sin_port);
    } else if (addr->ss_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)addr;
        inet_ntop(AF_INET6, &v6->sin6_addr, ip, sizeof(ip));
        port = ntohs(v6->sin6_port);
    } else {
        return 0;
    }
    lua_newtable(L);
    lua_pushstring(L, ip);
    lua_setfield(L, -2, "ip");
    lua_pushinteger(L, port);
    lua_setfield(L, -2, "port");
    return 1;
}

static int tcp_local(lua_State *L)
{
    lua_net_tcp_ud_t *ud = tcp_check(L, 1);
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (ud->is_tls) {
        lua_pushnil(L);
        lua_pushliteral(L, "not supported for TLS connections");
        return 2;
    }
    if (getsockname(ud->fd, (struct sockaddr *)&addr, &len) != 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "getsockname failed");
        return 2;
    }
    return push_sockaddr(L, &addr);
}

static int tcp_remote(lua_State *L)
{
    lua_net_tcp_ud_t *ud = tcp_check(L, 1);
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (ud->is_tls) {
        lua_pushnil(L);
        lua_pushliteral(L, "not supported for TLS connections");
        return 2;
    }
    if (getpeername(ud->fd, (struct sockaddr *)&addr, &len) != 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "getpeername failed");
        return 2;
    }
    return push_sockaddr(L, &addr);
}

static int tcp_shutdown(lua_State *L)
{
    lua_net_tcp_ud_t *ud = tcp_check(L, 1);
    const char *how = luaL_optstring(L, 2, "write");
    int mode;
    if (strcmp(how, "read") == 0) {
        mode = SHUT_RD;
    } else if (strcmp(how, "write") == 0) {
        mode = SHUT_WR;
    } else if (strcmp(how, "both") == 0) {
        mode = SHUT_RDWR;
    } else {
        return luaL_error(L, "how must be 'read', 'write' or 'both'");
    }
    if (ud->is_tls) {
        lua_pushnil(L);
        lua_pushliteral(L, "not supported for TLS connections");
        return 2;
    }
    if (shutdown(ud->fd, mode) != 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "shutdown failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int tcp_is_open(lua_State *L)
{
    lua_net_tcp_ud_t *ud = luaL_checkudata(L, 1, TCP_CONN_MT);
    lua_pushboolean(L, ud && ud->open);
    return 1;
}

static int tcp_close(lua_State *L)
{
    lua_net_tcp_ud_t *ud = luaL_checkudata(L, 1, TCP_CONN_MT);
    tcp_release(L, ud);
    return 0;
}

static int tcp_gc(lua_State *L)
{
    lua_net_tcp_ud_t *ud = luaL_testudata(L, 1, TCP_CONN_MT);
    if (ud) {
        tcp_release(L, ud);
    }
    return 0;
}

static int tcp_srv_close(lua_State *L)
{
    lua_net_tcp_srv_ud_t *ud = luaL_checkudata(L, 1, TCP_SRV_MT);
    if (ud && ud->open) {
        ud->open = false;
        close(ud->fd);
        ud->fd = -1;
        if (ud->budget_socket) {
            lua_net_budget_release(L, LUA_NET_KIND_SOCKET);
            ud->budget_socket = false;
        }
    }
    return 0;
}

static int tcp_srv_gc(lua_State *L)
{
    lua_net_tcp_srv_ud_t *ud = luaL_testudata(L, 1, TCP_SRV_MT);
    if (ud && ud->open) {
        ud->open = false;
        close(ud->fd);
        ud->fd = -1;
        if (ud->budget_socket) {
            lua_net_budget_release(L, LUA_NET_KIND_SOCKET);
            ud->budget_socket = false;
        }
    }
    return 0;
}

static int tcp_accept(lua_State *L)
{
    lua_net_tcp_srv_ud_t *srv = tcp_srv_check(L, 1);
    int timeout = tcp_timeout_opt(L, 2, srv->default_timeout_ms);
    int64_t deadline = lua_net_deadline(L, timeout);
    const char *err = NULL;

    while (true) {
        int ready = lua_net_wait_fd(L, srv->fd, false, deadline);
        if (ready <= 0) {
            lua_pushnil(L);
            lua_pushstring(L, ready == 0 ? "timeout" : (ready == -1 ? "stopped" : "io error"));
            return 2;
        }
        struct sockaddr_storage addr;
        socklen_t addr_len = sizeof(addr);
        int fd = accept(srv->fd, (struct sockaddr *)&addr, &addr_len);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            lua_pushnil(L);
            lua_pushliteral(L, "accept failed");
            return 2;
        }
        if (!lua_net_budget_acquire(L, LUA_NET_KIND_SOCKET, &err)) {
            close(fd);
            lua_pushnil(L);
            lua_pushstring(L, err);
            return 2;
        }
        if (!tcp_set_nonblock(fd)) {
            close(fd);
            lua_net_budget_release(L, LUA_NET_KIND_SOCKET);
            lua_pushnil(L);
            lua_pushliteral(L, "fcntl failed");
            return 2;
        }
        lua_net_tcp_ud_t *ud = lua_newuserdata(L, sizeof(*ud));
        memset(ud, 0, sizeof(*ud));
        ud->fd = fd;
        ud->open = true;
        ud->budget_socket = true;
        ud->default_timeout_ms = srv->default_timeout_ms;
        luaL_setmetatable(L, TCP_CONN_MT);
        return 1;
    }
}

static int tcp_srv_local(lua_State *L)
{
    lua_net_tcp_srv_ud_t *srv = tcp_srv_check(L, 1);
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (getsockname(srv->fd, (struct sockaddr *)&addr, &len) != 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "getsockname failed");
        return 2;
    }
    return push_sockaddr(L, &addr);
}

static int tcp_listen(lua_State *L)
{
    int port = (int)luaL_checkinteger(L, 1);
    int timeout = tcp_timeout_opt(L, 2, LUA_NET_DEF_TIMEOUT_MS);
    int backlog = 4;
    bool reuse = true;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "backlog");
        if (lua_isinteger(L, -1)) {
            backlog = (int)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "reuse");
        if (lua_isboolean(L, -1)) {
            reuse = lua_toboolean(L, -1);
        }
        lua_pop(L, 1);
    }
    if (port < 0 || port > 65535) {
        return luaL_error(L, "port must be 0..65535 (0 = ephemeral)");
    }

    const char *err = NULL;
    if (!lua_net_budget_acquire(L, LUA_NET_KIND_SOCKET, &err)) {
        lua_pushnil(L);
        lua_pushstring(L, err);
        return 2;
    }

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
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
    if (listen(fd, backlog > 0 ? backlog : 4) != 0 || !tcp_set_nonblock(fd)) {
        close(fd);
        lua_net_budget_release(L, LUA_NET_KIND_SOCKET);
        lua_pushnil(L);
        lua_pushliteral(L, "listen failed");
        return 2;
    }

    lua_net_tcp_srv_ud_t *ud = lua_newuserdata(L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    ud->fd = fd;
    ud->open = true;
    ud->budget_socket = true;
    ud->default_timeout_ms = timeout;
    luaL_setmetatable(L, TCP_SRV_MT);
    return 1;
}

static int tcp_connect(lua_State *L)
{
    const char *host = luaL_checkstring(L, 1);
    int port = (int)luaL_checkinteger(L, 2);
    int timeout = tcp_timeout_opt(L, 3, LUA_NET_DEF_TIMEOUT_MS);
    bool use_tls = tcp_opt_bool(L, 3, "tls");
    bool insecure = tcp_opt_bool(L, 3, "insecure");
    lua_getfield(L, 3, "ca_pem");
    const char *ca_pem = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
    lua_getfield(L, 3, "server_name");
    const char *server_name = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
    /* Keep both option strings referenced on the stack until the end. */
    int saved = lua_gettop(L);

    if (port <= 0 || port > 65535) {
        return luaL_error(L, "port must be 1..65535");
    }

    struct sockaddr_storage addr;
    socklen_t addr_len = 0;
    const char *err = NULL;
    if (!tcp_resolve_addr(L, host, port, &addr, &addr_len, timeout, &err)) {
        lua_pushnil(L);
        lua_pushstring(L, err ? err : "dns failed");
        return 2;
    }

    if (!lua_net_budget_acquire(L, LUA_NET_KIND_SOCKET, &err)) {
        lua_pushnil(L);
        lua_pushstring(L, err);
        return 2;
    }
    bool budget_tls = false;
    if (use_tls) {
        if (!lua_net_budget_acquire(L, LUA_NET_KIND_TLS, &err)) {
            lua_net_budget_release(L, LUA_NET_KIND_SOCKET);
            lua_pushnil(L);
            lua_pushstring(L, err);
            return 2;
        }
        budget_tls = true;
    }

    lua_net_tcp_ud_t *ud = lua_newuserdata(L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    ud->fd = -1;
    ud->default_timeout_ms = timeout;
    ud->is_tls = use_tls;
    ud->budget_socket = true;
    ud->budget_tls = budget_tls;

    if (use_tls) {
        char host_str[LUA_NET_HOST_MAX];
        strlcpy(host_str, host, sizeof(host_str));
        const char *tls_err = NULL;
        esp_tls_t *tls = lua_net_tls_connect(L, host_str, port, insecure, ca_pem,
                                             server_name, timeout, &tls_err);
        if (!tls) {
            ud->open = false;
            tcp_release(L, ud);
            lua_pushnil(L);
            lua_pushstring(L, tls_err ? tls_err : "TLS handshake failed");
            return 2;
        }
        int fd = -1;
        if (esp_tls_get_conn_sockfd(tls, &fd) != ESP_OK || fd < 0) {
            lua_net_tls_close(tls);
            ud->open = false;
            tcp_release(L, ud);
            lua_pushnil(L);
            lua_pushliteral(L, "TLS socket unavailable");
            return 2;
        }
        ud->tls = tls;
        ud->fd = fd;
        ud->open = true;
        luaL_setmetatable(L, TCP_CONN_MT);
        (void)saved;
        return 1;
    }

    int fd = socket(addr.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0 || !tcp_set_nonblock(fd)) {
        if (fd >= 0) {
            close(fd);
        }
        ud->open = false;
        tcp_release(L, ud);
        lua_pushnil(L);
        lua_pushliteral(L, "socket failed");
        return 2;
    }
    ud->fd = fd;

    int rc = connect(fd, (struct sockaddr *)&addr, addr_len);
    if (rc != 0 && errno != EINPROGRESS) {
        ud->open = false;
        tcp_release(L, ud);
        lua_pushnil(L);
        lua_pushliteral(L, "connect failed");
        return 2;
    }
    if (rc != 0) {
        int ready = lua_net_wait_fd(L, fd, true, lua_net_deadline(L, timeout));
        if (ready <= 0) {
            ud->open = false;
            tcp_release(L, ud);
            lua_pushnil(L);
            lua_pushstring(L, ready == 0 ? "timeout" : (ready == -1 ? "stopped" : "io error"));
            return 2;
        }
        int so_error = 0;
        socklen_t so_len = sizeof(so_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) != 0 || so_error != 0) {
            ud->open = false;
            tcp_release(L, ud);
            lua_pushnil(L);
            lua_pushliteral(L, "connection refused");
            return 2;
        }
    }
    ud->open = true;
    luaL_setmetatable(L, TCP_CONN_MT);
    return 1;
}

static const luaL_Reg tcp_conn_methods[] = {
    {"send", tcp_send},
    {"send_all", tcp_send_all},
    {"recv", tcp_recv},
    {"recv_exact", tcp_recv_exact},
    {"recv_line", tcp_recv_line},
    {"wait_readable", tcp_wait_readable},
    {"wait_writable", tcp_wait_writable},
    {"set_timeout", tcp_set_timeout},
    {"set_nodelay", tcp_set_nodelay},
    {"set_keepalive", tcp_set_keepalive},
    {"local_addr", tcp_local},
    {"remote", tcp_remote},
    {"shutdown", tcp_shutdown},
    {"is_open", tcp_is_open},
    {"close", tcp_close},
    {NULL, NULL},
};

static const luaL_Reg tcp_srv_methods[] = {
    {"accept", tcp_accept},
    {"local_addr", tcp_srv_local},
    {"close", tcp_srv_close},
    {NULL, NULL},
};

static void tcp_register_metatables(lua_State *L)
{
    if (luaL_newmetatable(L, TCP_CONN_MT)) {
        lua_pushcfunction(L, tcp_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushcfunction(L, tcp_close);
        lua_setfield(L, -2, "__close");
        lua_newtable(L);
        luaL_setfuncs(L, tcp_conn_methods, 0);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    if (luaL_newmetatable(L, TCP_SRV_MT)) {
        lua_pushcfunction(L, tcp_srv_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushcfunction(L, tcp_srv_close);
        lua_setfield(L, -2, "__close");
        lua_newtable(L);
        luaL_setfuncs(L, tcp_srv_methods, 0);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
}

int lua_net_tcp_module(lua_State *L)
{
    tcp_register_metatables(L);
    lua_newtable(L);
    lua_pushcfunction(L, tcp_connect);
    lua_setfield(L, -2, "connect");
    lua_pushcfunction(L, tcp_listen);
    lua_setfield(L, -2, "listen");
    return 1;
}
