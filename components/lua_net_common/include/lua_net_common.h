/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared socket budget and abortable wait helpers for the Lua network
 * modules (net / mqtt / websocket). All entry points run on the Lua job task.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LUA_NET_DEF_TIMEOUT_MS  5000
#define LUA_NET_SLICE_MS        50

/* Caps shared by every Lua network module. Global limits keep the script
 * layer from starving the Web UI and the XiaoZhi voice link. */
#define LUA_NET_MAX_SOCKETS_GLOBAL 4
#define LUA_NET_MAX_SOCKETS_JOB    3
#define LUA_NET_MAX_TLS_GLOBAL     2
#define LUA_NET_MAX_TLS_JOB        1

typedef enum {
    LUA_NET_KIND_SOCKET = 0,
    LUA_NET_KIND_TLS,
    LUA_NET_KIND_COUNT,
} lua_net_kind_t;

/* Acquire a socket/TLS slot for the job owning L. On failure returns false
 * and sets *err to a stable string ("socket budget exhausted", ...). */
bool lua_net_budget_acquire(lua_State *L, lua_net_kind_t kind, const char **err);
void lua_net_budget_release(lua_State *L, lua_net_kind_t kind);

/* Deadline in ms (esp_timer epoch); 0 means "no deadline". The helper also
 * folds in the Lua job's own deadline so a sync run_lua with a 3 s cap cannot
 * be blocked far past it. */
int64_t lua_net_deadline(lua_State *L, int timeout_ms);
int lua_net_remaining_ms(int64_t deadline);

bool lua_net_stopped(lua_State *L);

/* Wait for fd readiness in slices, checking stop and deadline.
 * 1 ready, 0 deadline, -1 stopped, -2 select error. */
int lua_net_wait_fd(lua_State *L, int fd, bool for_write, int64_t deadline);

#ifdef __cplusplus
}
#endif
