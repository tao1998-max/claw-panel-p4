/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_net_common.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>

#include "cap_lua.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lwip/sockets.h"

static const char *TAG = "lua_net_common";

#define LUA_NET_BUDGET_ENTRIES 8

typedef struct {
    char job[CAP_LUA_JOB_ID_LEN];
    int counts[LUA_NET_KIND_COUNT];
} lua_net_budget_entry_t;

static lua_net_budget_entry_t s_budget[LUA_NET_BUDGET_ENTRIES];
static int s_budget_global[LUA_NET_KIND_COUNT];
static portMUX_TYPE s_budget_mux = portMUX_INITIALIZER_UNLOCKED;

static const char *budget_job_id(lua_State *L, char *fallback, size_t fallback_size)
{
    const char *job = cap_lua_runtime_job_id(L);
    if (job && job[0]) {
        return job;
    }
    snprintf(fallback, fallback_size, "local");
    return fallback;
}

static lua_net_budget_entry_t *budget_entry_locked(const char *job, bool create)
{
    lua_net_budget_entry_t *free_entry = NULL;

    for (size_t i = 0; i < LUA_NET_BUDGET_ENTRIES; i++) {
        if (s_budget[i].job[0] == '\0') {
            if (!free_entry) {
                free_entry = &s_budget[i];
            }
            continue;
        }
        if (strncmp(s_budget[i].job, job, sizeof(s_budget[i].job)) == 0) {
            return &s_budget[i];
        }
    }
    if (create && free_entry) {
        memset(free_entry, 0, sizeof(*free_entry));
        strlcpy(free_entry->job, job, sizeof(free_entry->job));
        return free_entry;
    }
    return NULL;
}

bool lua_net_budget_acquire(lua_State *L, lua_net_kind_t kind, const char **err)
{
    char fallback[CAP_LUA_JOB_ID_LEN];
    const char *job = budget_job_id(L, fallback, sizeof(fallback));
    bool ok = false;

    portENTER_CRITICAL(&s_budget_mux);
    if (kind == LUA_NET_KIND_TLS) {
        lua_net_budget_entry_t *entry = budget_entry_locked(job, true);
        if (!entry) {
            *err = "tls budget exhausted (job table)";
        } else if (s_budget_global[LUA_NET_KIND_TLS] >= LUA_NET_MAX_TLS_GLOBAL) {
            *err = "tls budget exhausted (global)";
        } else if (entry->counts[LUA_NET_KIND_TLS] >= LUA_NET_MAX_TLS_JOB) {
            *err = "tls budget exhausted (job)";
        } else {
            entry->counts[LUA_NET_KIND_TLS]++;
            s_budget_global[LUA_NET_KIND_TLS]++;
            ok = true;
        }
        portEXIT_CRITICAL(&s_budget_mux);
        return ok;
    }

    lua_net_budget_entry_t *entry = budget_entry_locked(job, true);
    if (!entry) {
        *err = "socket budget exhausted (job table)";
    } else if (s_budget_global[LUA_NET_KIND_SOCKET] >= LUA_NET_MAX_SOCKETS_GLOBAL) {
        *err = "socket budget exhausted (global)";
    } else if (entry->counts[LUA_NET_KIND_SOCKET] >= LUA_NET_MAX_SOCKETS_JOB) {
        *err = "socket budget exhausted (job)";
    } else {
        entry->counts[LUA_NET_KIND_SOCKET]++;
        s_budget_global[LUA_NET_KIND_SOCKET]++;
        ok = true;
    }
    portEXIT_CRITICAL(&s_budget_mux);
    return ok;
}

void lua_net_budget_release(lua_State *L, lua_net_kind_t kind)
{
    char fallback[CAP_LUA_JOB_ID_LEN];
    const char *job = budget_job_id(L, fallback, sizeof(fallback));

    portENTER_CRITICAL(&s_budget_mux);
    if (s_budget_global[kind] > 0) {
        s_budget_global[kind]--;
    }
    lua_net_budget_entry_t *entry = budget_entry_locked(job, false);
    if (entry) {
        if (entry->counts[kind] > 0) {
            entry->counts[kind]--;
        }
        if (entry->counts[LUA_NET_KIND_SOCKET] == 0 && entry->counts[LUA_NET_KIND_TLS] == 0) {
            entry->job[0] = '\0';
        }
    }
    portEXIT_CRITICAL(&s_budget_mux);
}

int64_t lua_net_deadline(lua_State *L, int timeout_ms)
{
    int64_t deadline = 0;
    if (timeout_ms > 0) {
        deadline = esp_timer_get_time() / 1000 + timeout_ms;
    }
    int64_t job_us = cap_lua_runtime_deadline_us(L);
    if (job_us > 0) {
        int64_t job_ms = job_us / 1000;
        if (deadline == 0 || job_ms < deadline) {
            deadline = job_ms;
        }
    }
    return deadline;
}

int lua_net_remaining_ms(int64_t deadline)
{
    if (deadline == 0) {
        return INT32_MAX;
    }
    int64_t now = esp_timer_get_time() / 1000;
    if (now >= deadline) {
        return 0;
    }
    int64_t left = deadline - now;
    return left > INT32_MAX ? INT32_MAX : (int)left;
}

bool lua_net_stopped(lua_State *L)
{
    return cap_lua_runtime_stop_requested(L);
}

int lua_net_wait_fd(lua_State *L, int fd, bool for_write, int64_t deadline)
{
    while (true) {
        if (lua_net_stopped(L)) {
            return -1;
        }
        int remaining = lua_net_remaining_ms(deadline);
        if (remaining == 0) {
            return 0;
        }
        int wait = remaining > LUA_NET_SLICE_MS ? LUA_NET_SLICE_MS : remaining;

        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        struct timeval tv = {
            .tv_sec = wait / 1000,
            .tv_usec = (wait % 1000) * 1000,
        };
        int ret = select(fd + 1, for_write ? NULL : &set, for_write ? &set : NULL,
                         NULL, &tv);
        if (ret > 0) {
            return 1;
        }
        if (ret == 0) {
            continue;
        }
        if (errno == EINTR) {
            continue;
        }
        ESP_LOGW(TAG, "select failed: errno=%d", errno);
        return -2;
    }
}
