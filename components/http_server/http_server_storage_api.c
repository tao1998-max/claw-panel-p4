/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <sys/stat.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"

static const char *TAG = "http_server_storage_api";

static void add_volume(cJSON *root, const char *name, const char *mount)
{
    cJSON *volume = cJSON_AddObjectToObject(root, name);
    if (!volume) {
        return;
    }

    uint64_t total = 0;
    uint64_t free_bytes = 0;
    bool mounted = mount && mount[0] &&
                   esp_vfs_fat_info(mount, &total, &free_bytes) == ESP_OK;

    http_server_json_add_string(volume, "mount", mount ? mount : "");
    cJSON_AddBoolToObject(volume, "mounted", mounted);
    cJSON_AddNumberToObject(volume, "total", mounted ? (double)total : 0);
    cJSON_AddNumberToObject(volume, "free", mounted ? (double)free_bytes : 0);
    cJSON_AddNumberToObject(volume, "total_bytes", mounted ? (double)total : 0);
    cJSON_AddNumberToObject(volume, "free_bytes", mounted ? (double)free_bytes : 0);
    cJSON_AddNumberToObject(volume, "total_kb", mounted ? (double)(total / 1024) : 0);
    cJSON_AddNumberToObject(volume, "free_kb", mounted ? (double)(free_bytes / 1024) : 0);
}

static esp_err_t storage_handler(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    http_server_json_add_string(root, "storage_base_path", ctx->storage_base_path);
    add_volume(root, "fat", ctx->fat_mount_point);
    add_volume(root, "sd", ctx->sd_mount_point);

    cJSON *sd_assets = cJSON_AddObjectToObject(root, "sd_assets");
    if (sd_assets) {
        struct stat st = {0};
        bool exists = ctx->sd_assets_root[0] &&
                      stat(ctx->sd_assets_root, &st) == 0 && S_ISDIR(st.st_mode);
        http_server_json_add_string(sd_assets, "root", ctx->sd_assets_root);
        cJSON_AddBoolToObject(sd_assets, "exists", exists);
    }

    ESP_LOGD(TAG, "storage status requested");
    return http_server_send_json_response(req, root);
}

esp_err_t http_server_register_storage_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/storage", .method = HTTP_GET, .handler = storage_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
