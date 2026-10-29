/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * TLS (esp-tls) and URL helpers for the `net` module.
 */
#include "lua_module_net.h"

#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "lua_net";

/* ---------------- TLS ---------------- */

typedef struct {
    char host[LUA_NET_HOST_MAX];
    int port;
    bool insecure;
    const char *ca_pem;
    size_t ca_len;
    const char *server_name;
    int timeout_ms;
    esp_tls_t *tls;
    esp_err_t err;
    SemaphoreHandle_t done;
} lua_net_tls_job_t;

static void lua_net_tls_worker(void *arg)
{
    lua_net_tls_job_t *job = arg;
    esp_tls_cfg_t cfg = {0};

    cfg.timeout_ms = job->timeout_ms;
    /* Applies to the socket after the handshake: WANT_READ instead of a
     * blocking recv, which is what the abortable read loop needs. */
    cfg.non_block = true;
    if (job->insecure) {
        /* No verification option -> esp-tls skips verification when
         * CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY is enabled. Keep SNI/CN
         * untouched so the server still sees the right hostname. */
        cfg.skip_common_name = false;
    } else if (job->ca_pem && job->ca_len > 0) {
        cfg.cacert_buf = (const unsigned char *)job->ca_pem;
        cfg.cacert_bytes = (unsigned int)job->ca_len;
    } else {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    if (job->server_name && job->server_name[0]) {
        cfg.common_name = job->server_name;
    } else {
        cfg.common_name = job->host;
    }

    job->tls = esp_tls_init();
    if (!job->tls) {
        job->err = ESP_ERR_NO_MEM;
    } else if (!esp_tls_conn_new_sync(job->host, strlen(job->host), job->port,
                                      &cfg, job->tls)) {
        esp_tls_conn_destroy(job->tls);
        job->tls = NULL;
        job->err = ESP_FAIL;
    } else {
        job->err = ESP_OK;
    }

    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
}

esp_tls_t *lua_net_tls_connect(lua_State *L, const char *host, int port,
                               bool insecure, const char *ca_pem,
                               const char *server_name, int timeout_ms,
                               const char **err)
{
    (void)L;
    if (!host || !host[0] || port <= 0 || port > 65535) {
        *err = "invalid host or port";
        return NULL;
    }

    lua_net_tls_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        *err = "out of memory";
        return NULL;
    }
    strlcpy(job->host, host, sizeof(job->host));
    job->port = port;
    job->insecure = insecure;
    job->ca_pem = ca_pem;
    job->ca_len = ca_pem ? strlen(ca_pem) : 0;
    job->server_name = server_name;
    job->timeout_ms = timeout_ms > 0 ? timeout_ms : LUA_NET_DEF_TIMEOUT_MS;
    job->done = xSemaphoreCreateBinary();
    if (!job->done) {
        free(job);
        *err = "out of memory";
        return NULL;
    }

    if (xTaskCreate(lua_net_tls_worker, "net_tls", 10240, job, 5, NULL) != pdPASS) {
        vSemaphoreDelete(job->done);
        free(job);
        *err = "failed to start TLS task";
        return NULL;
    }

    /* The handshake is bounded by cfg.timeout_ms but not abortable. Wait a
     * little longer; on an (unlikely) hang we leak the small job struct
     * instead of freeing memory the worker may still touch. */
    if (xSemaphoreTake(job->done, pdMS_TO_TICKS(job->timeout_ms + 5000)) != pdTRUE) {
        *err = "TLS handshake timeout";
        return NULL;
    }
    esp_tls_t *tls = job->tls;
    esp_err_t job_err = job->err;
    vSemaphoreDelete(job->done);
    free(job);
    if (job_err != ESP_OK || !tls) {
        *err = "TLS handshake failed";
        return NULL;
    }
    *err = NULL;
    return tls;
}

int lua_net_tls_read(lua_State *L, esp_tls_t *tls, char *buf, size_t len,
                     int64_t deadline)
{
    while (true) {
        if (lua_net_stopped(L)) {
            return -1;
        }
        /* Read first: mbedTLS may hold decrypted records while the socket is
         * not readable. */
        int got = esp_tls_conn_read(tls, buf, (int)len);
        if (got > 0) {
            return got;
        }
        if (got == 0) {
            return 0;
        }
        bool want_write = (got == ESP_TLS_ERR_SSL_WANT_WRITE);
        if (got != ESP_TLS_ERR_SSL_WANT_READ && !want_write) {
            ESP_LOGW(TAG, "tls read error: %d", got);
            return -2;
        }
        /* Wait on the underlying socket for the direction mbedTLS asked for.
         * TLS reads need the fd; a WANT_WRITE state (renegotiation) wants it
         * writable. */
        int fd = -1;
        if (esp_tls_get_conn_sockfd(tls, &fd) != ESP_OK || fd < 0) {
            return -2;
        }
        int ready = lua_net_wait_fd(L, fd, want_write, deadline);
        if (ready <= 0) {
            return ready == 0 ? -3 : (ready == -1 ? -1 : -2);
        }
    }
}

int lua_net_tls_write(lua_State *L, esp_tls_t *tls, const char *buf, size_t len,
                      int64_t deadline)
{
    while (true) {
        if (lua_net_stopped(L)) {
            return -1;
        }
        if (lua_net_remaining_ms(deadline) == 0) {
            return -3;
        }
        int wrote = esp_tls_conn_write(tls, buf, (int)len);
        if (wrote > 0) {
            return wrote;
        }
        bool want_read = (wrote == ESP_TLS_ERR_SSL_WANT_READ);
        if (wrote != ESP_TLS_ERR_SSL_WANT_WRITE && !want_read) {
            ESP_LOGW(TAG, "tls write error: %d", wrote);
            return -2;
        }
        int fd = -1;
        if (esp_tls_get_conn_sockfd(tls, &fd) != ESP_OK || fd < 0) {
            return -2;
        }
        int ready = lua_net_wait_fd(L, fd, !want_read, deadline);
        if (ready <= 0) {
            return ready == 0 ? -3 : (ready == -1 ? -1 : -2);
        }
    }
}

void lua_net_tls_close(esp_tls_t *tls)
{
    if (tls) {
        esp_tls_conn_destroy(tls);
    }
}

/* ---------------- URL parsing ---------------- */

bool lua_net_parse_url(const char *url, lua_net_url_t *out)
{
    if (!url || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) {
        out->tls = true;
        out->port = 443;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        out->tls = false;
        out->port = 80;
        p += 7;
    } else {
        return false;
    }

    const char *authority_end = p + strcspn(p, "/?#");
    const char *host_begin = p;
    size_t host_len = 0;
    if (authority_end > p && p[0] == '[') {
        const char *close = memchr(p, ']', (size_t)(authority_end - p));
        if (!close) {
            return false;
        }
        host_begin = p + 1;
        host_len = (size_t)(close - host_begin);
        p = close + 1;
        if (p < authority_end && *p == ':') {
            p++;
            int port = 0;
            while (p < authority_end && *p >= '0' && *p <= '9') {
                port = port * 10 + (*p - '0');
                p++;
            }
            if (port > 0) {
                out->port = port;
            }
        }
    } else {
        const char *colon = memchr(p, ':', (size_t)(authority_end - p));
        host_len = colon ? (size_t)(colon - p) : (size_t)(authority_end - p);
        host_begin = p;
        if (colon) {
            int port = 0;
            for (const char *q = colon + 1; q < authority_end; q++) {
                if (*q < '0' || *q > '9') {
                    return false;
                }
                port = port * 10 + (*q - '0');
            }
            if (port > 0) {
                out->port = port;
            }
        }
    }
    if (host_len == 0 || host_len >= sizeof(out->host)) {
        return false;
    }
    memcpy(out->host, host_begin, host_len);
    out->host[host_len] = '\0';

    if (authority_end[0] == '\0') {
        strlcpy(out->path, "/", sizeof(out->path));
    } else if (authority_end[0] == '?') {
        snprintf(out->path, sizeof(out->path), "/%s", authority_end);
    } else {
        strlcpy(out->path, authority_end, sizeof(out->path));
    }
    return out->path[0] == '/';
}
