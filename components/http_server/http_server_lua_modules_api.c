/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include "cap_lua.h"
#include "cap_lua_internal.h"

static esp_err_t lua_modules_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    if (!root || !items) {
        cJSON_Delete(root);
        cJSON_Delete(items);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddItemToObject(root, "items", items);
    size_t count = cap_lua_get_module_count();
    for (size_t i = 0; i < count; i++) {
        const cap_lua_module_t *module = cap_lua_get_module(i);
        if (!module || !module->name) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            cJSON_Delete(root);
            httpd_resp_send_500(req);
            return ESP_ERR_NO_MEM;
        }
        http_server_json_add_string(item, "module_id", module->name);
        http_server_json_add_string(item, "display_name", module->name);
        cJSON_AddItemToArray(items, item);
    }

    return http_server_send_json_response(req, root);
}

esp_err_t http_server_register_lua_modules_routes(httpd_handle_t server)
{
    const httpd_uri_t handler = {
        .uri = "/api/lua-modules",
        .method = HTTP_GET,
        .handler = lua_modules_get_handler,
    };

    return httpd_register_uri_handler(server, &handler);
}
