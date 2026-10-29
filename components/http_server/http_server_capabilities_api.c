/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include "claw_cap.h"

static esp_err_t capabilities_get_handler(httpd_req_t *req)
{
    claw_cap_group_list_t groups = claw_cap_list_groups();
    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    if (!root || !items) {
        cJSON_Delete(root);
        cJSON_Delete(items);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddItemToObject(root, "items", items);
    for (size_t i = 0; i < groups.count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            cJSON_Delete(root);
            httpd_resp_send_500(req);
            return ESP_ERR_NO_MEM;
        }
        const char *id = groups.items[i].group_id;
        http_server_json_add_string(item, "group_id", id);
        http_server_json_add_string(item, "display_name",
                                    groups.items[i].plugin_name && groups.items[i].plugin_name[0]
                                        ? groups.items[i].plugin_name : id);
        cJSON_AddBoolToObject(item, "default_llm_visible", true);
        cJSON_AddItemToArray(items, item);
    }

    return http_server_send_json_response(req, root);
}

esp_err_t http_server_register_capabilities_routes(httpd_handle_t server)
{
    const httpd_uri_t handler = {
        .uri = "/api/capabilities",
        .method = HTTP_GET,
        .handler = capabilities_get_handler,
    };

    return httpd_register_uri_handler(server, &handler);
}
