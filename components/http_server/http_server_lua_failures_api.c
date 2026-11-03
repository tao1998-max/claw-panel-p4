/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Read-only debug endpoint for App / script authors (and external AI tools):
 * the most recent Lua job failures with their error text. This is the Web
 * equivalent of the serial "Lua job ... failed" warning and the task list's
 * red rows.
 */
#include "http_server_priv.h"

#include <stdlib.h>
#include <string.h>

#include "cap_lua.h"

static const char *status_name(cap_lua_job_status_t status)
{
    switch (status) {
    case CAP_LUA_JOB_FAILED:
        return "failed";
    case CAP_LUA_JOB_TIMEOUT:
        return "timeout";
    case CAP_LUA_JOB_STOPPED:
        return "stopped";
    default:
        return "unknown";
    }
}

static esp_err_t lua_failures_handler(httpd_req_t *req)
{
    int limit = 4;
    char limit_str[8] = {0};
    if (http_server_query_get(req, "limit", limit_str, sizeof(limit_str)) == ESP_OK && limit_str[0]) {
        int parsed = atoi(limit_str);
        if (parsed > 0) {
            limit = parsed > 4 ? 4 : parsed;
        }
    }

    cap_lua_failure_t failures[4] = {0};
    size_t count = cap_lua_get_recent_failures(failures, (size_t)limit);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "count", (double)count);
    cJSON *items = cJSON_CreateArray();
    if (!items) {
        cJSON_Delete(root);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToObject(root, "items", items);
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        http_server_json_add_string(item, "id", failures[i].job_id);
        http_server_json_add_string(item, "name", failures[i].name);
        http_server_json_add_string(item, "path", failures[i].path);
        http_server_json_add_string(item, "status", status_name(failures[i].status));
        http_server_json_add_string(item, "error", failures[i].error);
        cJSON_AddNumberToObject(item, "finished_at", (double)failures[i].finished_at);
        cJSON_AddItemToArray(items, item);
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    return http_server_send_json_response(req, root);
}

esp_err_t http_server_register_lua_failure_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/lua-failures", .method = HTTP_GET, .handler = lua_failures_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
