/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"

static const char *TAG = "http_server_sd_assets_api";

#define SD_ASSET_SPACE_RESERVE (256 * 1024)

static bool sd_is_mounted(uint64_t *total, uint64_t *free_bytes)
{
    http_server_ctx_t *ctx = http_server_ctx();
    uint64_t volume_total = 0;
    uint64_t volume_free = 0;
    bool mounted = ctx->sd_mount_point[0] &&
                   esp_vfs_fat_info(ctx->sd_mount_point, &volume_total, &volume_free) == ESP_OK;
    if (total) {
        *total = mounted ? volume_total : 0;
    }
    if (free_bytes) {
        *free_bytes = mounted ? volume_free : 0;
    }
    return mounted;
}

static esp_err_t send_json_error(httpd_req_t *req, const char *status,
                                 const char *code, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(root, "ok", false);
    http_server_json_add_string(root, "error", code);
    http_server_json_add_string(root, "message", message);
    httpd_resp_set_status(req, status);
    return http_server_send_json_response(req, root);
}

static esp_err_t send_ok(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/*
 * Client paths are relative to a fixed root. Reject anything that could
 * escape that root before composing the VFS path. Two roots exist:
 *   /api/sd-assets* -> <sd_mount>/panel-assets  (skill/asset contract)
 *   /api/sd-files*   -> <sd_mount>              (whole-card file management)
 */
static bool sd_path_is_safe(const char *path)
{
    if (!path || path[0] != '/' || strlen(path) >= HTTP_SERVER_PATH_MAX) {
        return false;
    }
    if (strstr(path, "..") != NULL || strchr(path, '\\') != NULL ||
        strchr(path, ':') != NULL) {
        return false;
    }
    if (strstr(path, "//") != NULL) {
        return false;
    }
    size_t len = strlen(path);
    if (len > 1 && path[len - 1] == '/') {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) {
            return false;
        }
    }
    return strcmp(path, "/.") != 0 && strstr(path, "/./") == NULL &&
           !(len >= 2 && strcmp(path + len - 2, "/.") == 0);
}

static const char *sd_root_for_uri(httpd_req_t *req)
{
    http_server_ctx_t *ctx = http_server_ctx();
    if (strncmp(req->uri, "/api/sd-files", strlen("/api/sd-files")) == 0 ||
        strncmp(req->uri, "/sd-files", strlen("/sd-files")) == 0) {
        return ctx->sd_mount_point;
    }
    return ctx->sd_assets_root;
}

static esp_err_t sd_resolve_path(const char *relative_path, const char *root,
                                 char *full_path, size_t full_path_size)
{
    if (!sd_path_is_safe(relative_path)) {
        return ESP_ERR_INVALID_ARG;
    }

    int written;
    if (strcmp(relative_path, "/") == 0) {
        written = snprintf(full_path, full_path_size, "%s", root);
    } else {
        written = snprintf(full_path, full_path_size, "%s%s", root, relative_path);
    }
    if (written <= 0 || (size_t)written >= full_path_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t root_len = strlen(root);
    if (strncmp(full_path, root, root_len) != 0 ||
        (full_path[root_len] != '\0' && full_path[root_len] != '/')) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t sd_ensure_root(void)
{
    http_server_ctx_t *ctx = http_server_ctx();
    if (!sd_is_mounted(NULL, NULL)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mkdir(ctx->sd_assets_root, 0775) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }
    struct stat st = {0};
    if (stat(ctx->sd_assets_root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static const char *sd_content_type(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) {
        return "application/octet-stream";
    }
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcasecmp(dot, ".ogg") == 0) {
        return "audio/ogg";
    }
    if (strcasecmp(dot, ".json") == 0) {
        return "application/json";
    }
    if (strcasecmp(dot, ".txt") == 0) {
        return "text/plain";
    }
    return "application/octet-stream";
}

static esp_err_t sd_list_handler(httpd_req_t *req)
{
    char relative_path[HTTP_SERVER_PATH_MAX] = "/";
    if (http_server_query_get(req, "path", relative_path, sizeof(relative_path)) != ESP_OK) {
        strlcpy(relative_path, "/", sizeof(relative_path));
    }

    if (!sd_is_mounted(NULL, NULL)) {
        return send_json_error(req, "503 Service Unavailable", "sd_not_mounted",
                               "SD card is not mounted");
    }

    const char *root = sd_root_for_uri(req);
    char full_path[HTTP_SERVER_PATH_MAX];
    if (sd_resolve_path(relative_path, root, full_path, sizeof(full_path)) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "invalid_path", "Invalid path");
    }

    if (strcmp(root, http_server_ctx()->sd_assets_root) == 0 && sd_ensure_root() != ESP_OK) {
        return send_json_error(req, "500 Internal Server Error", "io_error",
                               "Failed to access SD asset root");
    }

    DIR *dir = opendir(full_path);
    if (!dir) {
        if (errno == ENOENT && strcmp(relative_path, "/") == 0) {
            cJSON *root_obj = cJSON_CreateObject();
            cJSON *entries = cJSON_CreateArray();
            if (!root_obj || !entries) {
                cJSON_Delete(root_obj);
                cJSON_Delete(entries);
                httpd_resp_send_500(req);
                return ESP_ERR_NO_MEM;
            }
            http_server_json_add_string(root_obj, "root", root);
            http_server_json_add_string(root_obj, "path", relative_path);
            cJSON_AddBoolToObject(root_obj, "exists", false);
            cJSON_AddItemToObject(root_obj, "entries", entries);
            return http_server_send_json_response(req, root_obj);
        }
        return send_json_error(req, "404 Not Found", "not_found", "Directory not found");
    }

    cJSON *root_obj = cJSON_CreateObject();
    cJSON *entries = cJSON_CreateArray();
    if (!root_obj || !entries) {
        closedir(dir);
        cJSON_Delete(root_obj);
        cJSON_Delete(entries);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    http_server_json_add_string(root_obj, "root", root);
    http_server_json_add_string(root_obj, "path", relative_path);
    cJSON_AddBoolToObject(root_obj, "exists", true);
    cJSON_AddItemToObject(root_obj, "entries", entries);

    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child_relative[HTTP_SERVER_PATH_MAX];
        char child_full[HTTP_SERVER_PATH_MAX];
        if (!http_server_build_child_relative_path(relative_path, entry->d_name,
                                                   child_relative, sizeof(child_relative)) ||
            sd_resolve_path(child_relative, root, child_full, sizeof(child_full)) != ESP_OK) {
            continue;
        }

        struct stat st = {0};
        if (stat(child_full, &st) != 0) {
            continue;
        }

        cJSON *item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        http_server_json_add_string(item, "name", entry->d_name);
        http_server_json_add_string(item, "path", child_relative);
        cJSON_AddBoolToObject(item, "is_dir", S_ISDIR(st.st_mode));
        cJSON_AddNumberToObject(item, "size", S_ISDIR(st.st_mode) ? 0 : (double)st.st_size);
        cJSON_AddItemToArray(entries, item);
    }

    closedir(dir);
    return http_server_send_json_response(req, root_obj);
}

static esp_err_t sd_stream_file(httpd_req_t *req, const char *full_path)
{
    FILE *file = fopen(full_path, "rb");
    if (!file) {
        return send_json_error(req, "500 Internal Server Error", "io_error",
                               "Failed to open file");
    }

    char *scratch = http_server_alloc_scratch_buffer();
    if (!scratch) {
        fclose(file);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }

    httpd_resp_set_type(req, sd_content_type(full_path));
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    while (!feof(file)) {
        size_t read_bytes = fread(scratch, 1, HTTP_SERVER_SCRATCH_SIZE, file);
        if (read_bytes == 0) {
            if (ferror(file)) {
                free(scratch);
                fclose(file);
                return ESP_FAIL;
            }
            break;
        }
        if (httpd_resp_send_chunk(req, scratch, read_bytes) != ESP_OK) {
            free(scratch);
            fclose(file);
            return ESP_FAIL;
        }
    }

    free(scratch);
    fclose(file);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t sd_download_handler(httpd_req_t *req)
{
    char relative_path[HTTP_SERVER_PATH_MAX] = {0};
    if (http_server_query_get(req, "path", relative_path, sizeof(relative_path)) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "invalid_path", "Missing path");
    }
    if (!sd_is_mounted(NULL, NULL)) {
        return send_json_error(req, "503 Service Unavailable", "sd_not_mounted",
                               "SD card is not mounted");
    }

    const char *root = sd_root_for_uri(req);
    char full_path[HTTP_SERVER_PATH_MAX];
    if (sd_resolve_path(relative_path, root, full_path, sizeof(full_path)) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "invalid_path", "Invalid path");
    }

    struct stat st = {0};
    if (stat(full_path, &st) != 0 || S_ISDIR(st.st_mode)) {
        return send_json_error(req, "404 Not Found", "not_found", "File not found");
    }

    return sd_stream_file(req, full_path);
}

/* Browser-friendly download: GET /sd-files/<path>, mirroring the files API. */
static esp_err_t sd_download_wildcard_handler(httpd_req_t *req)
{
    const char *relative_path = req->uri + strlen("/sd-files");
    if (!sd_path_is_safe(relative_path)) {
        return send_json_error(req, "400 Bad Request", "invalid_path", "Invalid path");
    }
    if (!sd_is_mounted(NULL, NULL)) {
        return send_json_error(req, "503 Service Unavailable", "sd_not_mounted",
                               "SD card is not mounted");
    }

    char full_path[HTTP_SERVER_PATH_MAX];
    if (sd_resolve_path(relative_path, http_server_ctx()->sd_mount_point,
                        full_path, sizeof(full_path)) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "invalid_path", "Invalid path");
    }

    struct stat st = {0};
    if (stat(full_path, &st) != 0 || S_ISDIR(st.st_mode)) {
        return send_json_error(req, "404 Not Found", "not_found", "File not found");
    }

    return sd_stream_file(req, full_path);
}

static esp_err_t sd_mkdir_handler(httpd_req_t *req)
{
    cJSON *root = NULL;
    if (http_server_parse_json_body(req, &root) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "invalid_request", "Invalid JSON body");
    }

    cJSON *path_item = cJSON_GetObjectItemCaseSensitive(root, "path");
    if (!cJSON_IsString(path_item) || !sd_path_is_safe(path_item->valuestring)) {
        cJSON_Delete(root);
        return send_json_error(req, "400 Bad Request", "invalid_path", "Invalid path");
    }

    cJSON *rec_item = cJSON_GetObjectItemCaseSensitive(root, "recursive");
    bool recursive = cJSON_IsBool(rec_item) && cJSON_IsTrue(rec_item);

    char relative_path[HTTP_SERVER_PATH_MAX];
    strlcpy(relative_path, path_item->valuestring, sizeof(relative_path));
    cJSON_Delete(root);

    if (!sd_is_mounted(NULL, NULL)) {
        return send_json_error(req, "503 Service Unavailable", "sd_not_mounted",
                               "SD card is not mounted");
    }

    const char *sd_root = sd_root_for_uri(req);
    char full_path[HTTP_SERVER_PATH_MAX];
    if (sd_resolve_path(relative_path, sd_root, full_path, sizeof(full_path)) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "invalid_path", "Invalid path");
    }

    if (strcmp(relative_path, "/") == 0) {
        if (strcmp(sd_root, http_server_ctx()->sd_assets_root) != 0) {
            /* /api/sd-files: the mount point itself — already verified mounted. */
            return send_ok(req);
        }
        return sd_ensure_root() == ESP_OK ? send_ok(req) :
               send_json_error(req, "500 Internal Server Error", "io_error",
                               "Failed to create SD asset root");
    }

    if (recursive) {
        if (http_server_mkdir_parents(full_path, 0775) != 0) {
            return send_json_error(req, "500 Internal Server Error", "io_error",
                                   "Failed to create directory");
        }
    } else if (mkdir(full_path, 0775) != 0 && errno != EEXIST) {
        return send_json_error(req, "500 Internal Server Error", "io_error",
                               "Failed to create directory");
    }
    return send_ok(req);
}

static esp_err_t sd_upload_handler(httpd_req_t *req)
{
    char relative_path[HTTP_SERVER_PATH_MAX] = {0};
    if (http_server_query_get(req, "path", relative_path, sizeof(relative_path)) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "invalid_path", "Missing path");
    }
    if (strcmp(relative_path, "/") == 0) {
        return send_json_error(req, "400 Bad Request", "invalid_path",
                               "Cannot upload to the SD asset root");
    }
    if (!sd_path_is_safe(relative_path)) {
        return send_json_error(req, "400 Bad Request", "invalid_path", "Invalid path");
    }
    if (req->content_len > HTTP_SERVER_UPLOAD_MAX_SIZE) {
        return send_json_error(req, "413 Payload Too Large", "content_too_large",
                               "Upload is too large");
    }

    uint64_t total = 0;
    uint64_t free_bytes = 0;
    if (!sd_is_mounted(&total, &free_bytes)) {
        return send_json_error(req, "503 Service Unavailable", "sd_not_mounted",
                               "SD card is not mounted");
    }
    if ((uint64_t)req->content_len + SD_ASSET_SPACE_RESERVE > free_bytes) {
        return send_json_error(req, "507 Insufficient Storage", "insufficient_space",
                               "Not enough free space on the SD card");
    }

    char full_path[HTTP_SERVER_PATH_MAX];
    if (sd_resolve_path(relative_path, sd_root_for_uri(req),
                        full_path, sizeof(full_path)) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "invalid_path", "Invalid path");
    }

    char parent_path[HTTP_SERVER_PATH_MAX];
    strlcpy(parent_path, full_path, sizeof(parent_path));
    char *slash = strrchr(parent_path, '/');
    if (!slash || slash == parent_path) {
        return send_json_error(req, "400 Bad Request", "invalid_path", "Invalid path");
    }
    *slash = '\0';

    struct stat st = {0};
    if (stat(parent_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return send_json_error(req, "400 Bad Request", "parent_not_found",
                               "Parent directory not found");
    }

    FILE *file = fopen(full_path, "wb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to create %s: errno=%d", full_path, errno);
        return send_json_error(req, "500 Internal Server Error", "io_error",
                               "Failed to create file");
    }

    char *scratch = NULL;
    if (req->content_len > 0) {
        scratch = http_server_alloc_scratch_buffer();
        if (!scratch) {
            fclose(file);
            unlink(full_path);
            httpd_resp_send_500(req);
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "Uploading %zu bytes to %s", req->content_len, full_path);
    size_t remaining = req->content_len;
    while (remaining > 0) {
        size_t chunk = remaining > HTTP_SERVER_SCRATCH_SIZE ? HTTP_SERVER_SCRATCH_SIZE : remaining;
        int received = httpd_req_recv(req, scratch, chunk);
        if (received <= 0) {
            ESP_LOGE(TAG, "Receive failed for %s: error=%d", full_path, received);
            free(scratch);
            fclose(file);
            unlink(full_path);
            return send_json_error(req, "500 Internal Server Error", "io_error",
                                   "Upload failed");
        }
        size_t written = fwrite(scratch, 1, received, file);
        if (written != (size_t)received) {
            ESP_LOGE(TAG, "Write failed for %s: written=%u expected=%d errno=%d",
                     full_path, (unsigned)written, received, errno);
            free(scratch);
            fclose(file);
            unlink(full_path);
            return send_json_error(req, "500 Internal Server Error", "io_error",
                                   "Upload failed");
        }
        remaining -= (size_t)received;
    }

    free(scratch);
    if (fclose(file) != 0) {
        ESP_LOGE(TAG, "Failed to close %s: errno=%d", full_path, errno);
        unlink(full_path);
        return send_json_error(req, "500 Internal Server Error", "io_error",
                               "Upload failed");
    }
    return send_ok(req);
}

static esp_err_t sd_delete_handler(httpd_req_t *req)
{
    char relative_path[HTTP_SERVER_PATH_MAX] = {0};
    if (http_server_query_get(req, "path", relative_path, sizeof(relative_path)) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "invalid_path", "Missing path");
    }
    if (strcmp(relative_path, "/") == 0) {
        return send_json_error(req, "400 Bad Request", "root_protected",
                               "Cannot delete the SD asset root");
    }
    if (!sd_path_is_safe(relative_path)) {
        return send_json_error(req, "400 Bad Request", "invalid_path", "Invalid path");
    }
    if (!sd_is_mounted(NULL, NULL)) {
        return send_json_error(req, "503 Service Unavailable", "sd_not_mounted",
                               "SD card is not mounted");
    }

    char recursive_str[8] = {0};
    http_server_query_get(req, "recursive", recursive_str, sizeof(recursive_str));
    bool recursive = strcmp(recursive_str, "1") == 0 || strcmp(recursive_str, "true") == 0;

    char full_path[HTTP_SERVER_PATH_MAX];
    if (sd_resolve_path(relative_path, sd_root_for_uri(req),
                        full_path, sizeof(full_path)) != ESP_OK) {
        return send_json_error(req, "400 Bad Request", "invalid_path", "Invalid path");
    }

    struct stat st = {0};
    if (stat(full_path, &st) != 0) {
        return send_json_error(req, "404 Not Found", "not_found", "Path not found");
    }

    if (S_ISDIR(st.st_mode)) {
        if (recursive) {
            if (http_server_remove_tree(full_path) != 0) {
                return send_json_error(req, "500 Internal Server Error", "io_error",
                                       "Recursive delete failed");
            }
        } else {
            DIR *dir = opendir(full_path);
            if (dir) {
                bool has_children = false;
                struct dirent *de;
                while ((de = readdir(dir)) != NULL) {
                    if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0) {
                        has_children = true;
                        break;
                    }
                }
                closedir(dir);
                if (has_children) {
                    return send_json_error(req, "409 Conflict", "directory_not_empty",
                                           "Directory is not empty. Use recursive=1.");
                }
            }
            if (rmdir(full_path) != 0) {
                return send_json_error(req, "500 Internal Server Error", "io_error",
                                       "Delete failed");
            }
        }
    } else if (unlink(full_path) != 0) {
        return send_json_error(req, "500 Internal Server Error", "io_error",
                               "Delete failed");
    }

    return send_ok(req);
}

esp_err_t http_server_register_sd_assets_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        /* Restricted asset tree: <sd_mount>/panel-assets */
        { .uri = "/api/sd-assets", .method = HTTP_GET, .handler = sd_list_handler },
        { .uri = "/api/sd-assets", .method = HTTP_DELETE, .handler = sd_delete_handler },
        { .uri = "/api/sd-assets/file", .method = HTTP_GET, .handler = sd_download_handler },
        { .uri = "/api/sd-assets/mkdir", .method = HTTP_POST, .handler = sd_mkdir_handler },
        { .uri = "/api/sd-assets/upload", .method = HTTP_POST, .handler = sd_upload_handler },
        /* Whole-card file management (Web UI SD tab) */
        { .uri = "/api/sd-files", .method = HTTP_GET, .handler = sd_list_handler },
        { .uri = "/api/sd-files", .method = HTTP_DELETE, .handler = sd_delete_handler },
        { .uri = "/api/sd-files/file", .method = HTTP_GET, .handler = sd_download_handler },
        { .uri = "/api/sd-files/mkdir", .method = HTTP_POST, .handler = sd_mkdir_handler },
        { .uri = "/api/sd-files/upload", .method = HTTP_POST, .handler = sd_upload_handler },
        { .uri = "/sd-files/*", .method = HTTP_GET, .handler = sd_download_wildcard_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
