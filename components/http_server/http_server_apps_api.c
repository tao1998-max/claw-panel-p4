/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * App launch / self-test API (the closed loop for external AI tools):
 *
 *   GET  /api/apps                         list registered Apps (FAT + SD)
 *   POST /api/apps/refresh                 rescan the app roots (no reboot)
 *   POST /api/apps/<id>/launch             start an App, returns job_id
 *   POST /api/apps/<id>/test?duration_ms=N start, run N ms, stop, report
 *   GET  /api/lua-tasks                    currently running Lua jobs
 *   POST /api/lua-tasks/<id>/stop          stop a running job
 *
 * `/api/apps/<id>/test` turns "launch it and watch the screen" into one HTTP
 * call: the response says whether the App crashed and carries the exact
 * `path:line: reason` from the Lua failure record.
 *
 * The httpd task stack is 8 KiB, so large structures live on the heap here.
 * The Web API has no authentication: these routes can run any registered App
 * (Lua with io/network access). LAN use only.
 */
#include "http_server_priv.h"

#include <stdlib.h>
#include <string.h>

#include "app_registry.h"
#include "cap_lua.h"
#include "claw_api.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define APPS_URI_PREFIX       "/api/apps/"
#define TASKS_URI_PREFIX      "/api/lua-tasks/"
#define APPS_ARGS_MAX         512
#define APPS_TEST_DEFAULT_MS  3000
#define APPS_TEST_MAX_MS      15000
#define APPS_TASK_SNAPSHOTS   4

typedef struct {
    char target[APP_REGISTRY_ID_MAX_LEN + 1];
    char app_id[APP_REGISTRY_ID_MAX_LEN + 1];
    char name[64];
    char entry[192];
    char args[APPS_ARGS_MAX];
    bool found;
} app_lookup_t;

typedef struct {
    cJSON *items;
} app_list_ctx_t;

static esp_err_t lookup_cb(const app_registry_entry_t *entry, void *user_ctx)
{
    app_lookup_t *out = user_ctx;
    if (out->found || !entry || !entry->app_id ||
        strcmp(entry->app_id, out->target) != 0) {
        return ESP_OK;
    }
    strlcpy(out->app_id, entry->app_id, sizeof(out->app_id));
    strlcpy(out->name, entry->display_name && entry->display_name[0] ?
            entry->display_name : entry->app_id, sizeof(out->name));
    strlcpy(out->entry, entry->entry ? entry->entry : "", sizeof(out->entry));
    strlcpy(out->args, entry->args_json && entry->args_json[0] ?
            entry->args_json : "{}", sizeof(out->args));
    out->found = true;
    return ESP_OK;
}

static app_lookup_t *lookup_app(const char *app_id)
{
    if (!app_id || !app_registry_id_is_valid(app_id)) {
        return NULL;
    }
    app_lookup_t *out = calloc(1, sizeof(*out));
    if (!out) {
        return NULL;
    }
    strlcpy(out->target, app_id, sizeof(out->target));
    (void)app_registry_foreach(lookup_cb, out);
    if (!out->found) {
        free(out);
        return NULL;
    }
    return out;
}

static esp_err_t send_error(httpd_req_t *req, const char *status,
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

static const char *status_name(cap_lua_job_status_t status)
{
    switch (status) {
    case CAP_LUA_JOB_QUEUED:
        return "queued";
    case CAP_LUA_JOB_RUNNING:
        return "running";
    case CAP_LUA_JOB_DONE:
        return "done";
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

/* Returns "failed"/"timeout" and copies the error when this job failed. */
static const char *failure_for_job(const char *job_id, char *error, size_t error_size)
{
    cap_lua_failure_t failures[4] = {0};
    size_t count = cap_lua_get_recent_failures(failures, 4);
    for (size_t i = 0; i < count; i++) {
        if (strcmp(failures[i].job_id, job_id) == 0) {
            strlcpy(error, failures[i].error, error_size);
            return failures[i].status == CAP_LUA_JOB_TIMEOUT ? "timeout" : "failed";
        }
    }
    return NULL;
}

static bool job_active(const char *job_id)
{
    cap_lua_job_snapshot_t *jobs = calloc(APPS_TASK_SNAPSHOTS, sizeof(*jobs));
    if (!jobs) {
        return false;
    }
    size_t count = cap_lua_collect_active_jobs(jobs, APPS_TASK_SNAPSHOTS);
    bool found = false;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(jobs[i].job_id, job_id) == 0) {
            found = true;
            break;
        }
    }
    free(jobs);
    return found;
}

/* A previous instance of the same App (a manual launch, or a test that failed
 * to stop) would make cap_lua reject the new job by name. Stop matches first so
 * launch/test are idempotent. */
static void stop_existing_instance(const app_lookup_t *app)
{
    cap_lua_job_snapshot_t *jobs = calloc(APPS_TASK_SNAPSHOTS, sizeof(*jobs));
    if (!jobs) {
        return;
    }
    size_t count = cap_lua_collect_active_jobs(jobs, APPS_TASK_SNAPSHOTS);
    for (size_t i = 0; i < count; i++) {
        if (strcmp(jobs[i].name, app->name) == 0 ||
            strcmp(jobs[i].path, app->entry) == 0) {
            char out[96] = {0};
            (void)claw_api_stop_task(jobs[i].job_id, 2000, out, sizeof(out));
        }
    }
    free(jobs);
}

static esp_err_t start_app(httpd_req_t *req, const app_lookup_t *app,
                           char *job_id, size_t job_id_size)
{
    char out[320] = {0};
    stop_existing_instance(app);
    esp_err_t err = claw_api_start_task(app->name, NULL, app->entry, app->args,
                                        0, out, sizeof(out));
    if (err != ESP_OK) {
        return send_error(req, "409 Conflict", "launch_failed",
                          out[0] ? out : esp_err_to_name(err));
    }
    cJSON *parsed = cJSON_Parse(out);
    cJSON *id = parsed ? cJSON_GetObjectItemCaseSensitive(parsed, "job_id") : NULL;
    bool ok = cJSON_IsString(id) && id->valuestring && id->valuestring[0];
    if (ok) {
        strlcpy(job_id, id->valuestring, job_id_size);
    }
    cJSON_Delete(parsed);
    if (!ok) {
        return send_error(req, "500 Internal Server Error", "launch_failed",
                          "device did not return a job id");
    }
    return ESP_OK;
}

static esp_err_t list_cb(const app_registry_entry_t *entry, void *user_ctx)
{
    app_list_ctx_t *ctx = user_ctx;
    if (!entry || !entry->app_id) {
        return ESP_OK;
    }
    cJSON *item = cJSON_CreateObject();
    if (!item) {
        return ESP_OK;
    }
    http_server_json_add_string(item, "id", entry->app_id);
    http_server_json_add_string(item, "name",
                                entry->display_name && entry->display_name[0] ?
                                entry->display_name : entry->app_id);
    http_server_json_add_string(item, "path", entry->entry ? entry->entry : "");
    http_server_json_add_string(item, "version", entry->version ? entry->version : "");
    http_server_json_add_string(item, "icon", entry->icon ? entry->icon : "");
    http_server_json_add_string(item, "volume",
                                entry->manage_mode == APP_REGISTRY_MANAGE_MODE_RUNTIME ?
                                "fat" : "sd");
    cJSON_AddItemToArray(ctx->items, item);
    return ESP_OK;
}

static esp_err_t apps_list_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    if (!root || !items) {
        cJSON_Delete(root);
        cJSON_Delete(items);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddItemToObject(root, "items", items);
    app_list_ctx_t ctx = { .items = items };
    (void)app_registry_foreach(list_cb, &ctx);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    return http_server_send_json_response(req, root);
}

static int parse_duration(httpd_req_t *req)
{
    char value[8] = {0};
    int duration = APPS_TEST_DEFAULT_MS;
    if (http_server_query_get(req, "duration_ms", value, sizeof(value)) == ESP_OK &&
        value[0]) {
        int parsed = atoi(value);
        if (parsed > 0) {
            duration = parsed;
        }
    }
    if (duration < 300) {
        duration = 300;
    }
    if (duration > APPS_TEST_MAX_MS) {
        duration = APPS_TEST_MAX_MS;
    }
    return duration;
}

static bool parse_app_uri(httpd_req_t *req, char *id, size_t id_size,
                          char *action, size_t action_size)
{
    const char *p = req->uri + strlen(APPS_URI_PREFIX);
    size_t id_len = strcspn(p, "/?");
    if (id_len == 0 || id_len >= id_size) {
        return false;
    }
    memcpy(id, p, id_len);
    id[id_len] = '\0';
    p += id_len;
    if (*p == '/') {
        p++;
    }
    size_t action_len = strcspn(p, "?");
    if (action_len >= action_size) {
        return false;
    }
    memcpy(action, p, action_len);
    action[action_len] = '\0';
    return true;
}

static esp_err_t apps_launch(httpd_req_t *req, const app_lookup_t *app)
{
    char job_id[CAP_LUA_JOB_ID_LEN] = {0};
    esp_err_t err = start_app(req, app, job_id, sizeof(job_id));
    if (err != ESP_OK) {
        return err;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    http_server_json_add_string(root, "id", app->app_id);
    http_server_json_add_string(root, "name", app->name);
    http_server_json_add_string(root, "job_id", job_id);
    return http_server_send_json_response(req, root);
}

static esp_err_t apps_test(httpd_req_t *req, const app_lookup_t *app)
{
    int duration_ms = parse_duration(req);
    char job_id[CAP_LUA_JOB_ID_LEN] = {0};
    esp_err_t err = start_app(req, app, job_id, sizeof(job_id));
    if (err != ESP_OK) {
        return err;
    }

    int64_t started_us = esp_timer_get_time();
    bool appeared = false;
    const char *result = "running";
    char error[192] = {0};
    while (true) {
        int64_t elapsed_ms = (esp_timer_get_time() - started_us) / 1000;
        if (job_active(job_id)) {
            appeared = true;
        } else {
            const char *failure = failure_for_job(job_id, error, sizeof(error));
            if (failure) {
                result = failure;
                break;
            }
            if (appeared || elapsed_ms > 1500) {
                result = "finished";
                break;
            }
        }
        if (elapsed_ms >= duration_ms) {
            char out[160] = {0};
            esp_err_t stop_err = claw_api_stop_task(job_id, 2000, out, sizeof(out));
            if (stop_err != ESP_OK) {
                char failure_err[192] = {0};
                if (failure_for_job(job_id, failure_err, sizeof(failure_err)) == NULL) {
                    strlcpy(error, out[0] ? out : esp_err_to_name(stop_err), sizeof(error));
                }
            }
            result = "running";
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    bool ok = strcmp(result, "failed") != 0 && strcmp(result, "timeout") != 0;
    cJSON_AddBoolToObject(root, "ok", ok);
    http_server_json_add_string(root, "id", app->app_id);
    http_server_json_add_string(root, "name", app->name);
    http_server_json_add_string(root, "job_id", job_id);
    http_server_json_add_string(root, "result", result);
    if (error[0]) {
        http_server_json_add_string(root, "error", error);
    }
    cJSON_AddNumberToObject(root, "duration_ms",
                            (double)((esp_timer_get_time() - started_us) / 1000));
    return http_server_send_json_response(req, root);
}

static esp_err_t apps_action_handler(httpd_req_t *req)
{
    char app_id[APP_REGISTRY_ID_MAX_LEN + 1];
    char action[16];
    if (!parse_app_uri(req, app_id, sizeof(app_id), action, sizeof(action))) {
        return send_error(req, "400 Bad Request", "invalid_request", "Invalid app id");
    }

    /* POST /api/apps/refresh rescans the FAT/SD app roots without a reboot. */
    if (strcmp(app_id, "refresh") == 0 && action[0] == '\0') {
        esp_err_t err = app_registry_reload();
        if (err != ESP_OK) {
            return send_error(req, "500 Internal Server Error", "reload_failed",
                              esp_err_to_name(err));
        }
        cJSON *root = cJSON_CreateObject();
        if (!root) {
            httpd_resp_send_500(req);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "message", "app registry reloaded");
        return http_server_send_json_response(req, root);
    }

    app_lookup_t *app = lookup_app(app_id);
    if (!app) {
        return send_error(req, "404 Not Found", "app_not_found",
                          "No registered App with this id");
    }

    esp_err_t err;
    if (strcmp(action, "launch") == 0) {
        err = apps_launch(req, app);
    } else if (strcmp(action, "test") == 0) {
        err = apps_test(req, app);
    } else {
        err = send_error(req, "404 Not Found", "invalid_action",
                         "Expected /launch or /test");
    }
    free(app);
    return err;
}

static esp_err_t tasks_list_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *items = cJSON_CreateArray();
    if (!root || !items) {
        cJSON_Delete(root);
        cJSON_Delete(items);
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddItemToObject(root, "items", items);

    cap_lua_job_snapshot_t *jobs = calloc(APPS_TASK_SNAPSHOTS, sizeof(*jobs));
    size_t count = jobs ? cap_lua_collect_active_jobs(jobs, APPS_TASK_SNAPSHOTS) : 0;
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        http_server_json_add_string(item, "id", jobs[i].job_id);
        http_server_json_add_string(item, "name", jobs[i].name);
        http_server_json_add_string(item, "path", jobs[i].path);
        http_server_json_add_string(item, "status", status_name(jobs[i].status));
        cJSON_AddItemToArray(items, item);
    }
    free(jobs);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, max-age=0");
    return http_server_send_json_response(req, root);
}

static esp_err_t tasks_action_handler(httpd_req_t *req)
{
    const char *p = req->uri + strlen(TASKS_URI_PREFIX);
    size_t id_len = strcspn(p, "/?");
    if (id_len == 0 || id_len >= CAP_LUA_JOB_ID_LEN) {
        return send_error(req, "400 Bad Request", "invalid_request", "Invalid job id");
    }
    char job_id[CAP_LUA_JOB_ID_LEN] = {0};
    memcpy(job_id, p, id_len);
    job_id[id_len] = '\0';
    p += id_len;
    if (*p == '/') {
        p++;
    }
    size_t action_len = strcspn(p, "?");
    if (action_len != 0 && !(action_len == 4 && strncmp(p, "stop", 4) == 0)) {
        return send_error(req, "404 Not Found", "invalid_action", "Expected /stop");
    }

    char out[160] = {0};
    esp_err_t err = claw_api_stop_task(job_id, 2000, out, sizeof(out));
    if (err != ESP_OK) {
        return send_error(req, "409 Conflict", "stop_failed",
                          out[0] ? out : esp_err_to_name(err));
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    http_server_json_add_string(root, "job_id", job_id);
    return http_server_send_json_response(req, root);
}

esp_err_t http_server_register_app_routes(httpd_handle_t server)
{
    const httpd_uri_t handlers[] = {
        { .uri = "/api/apps", .method = HTTP_GET, .handler = apps_list_handler },
        { .uri = "/api/apps/*", .method = HTTP_POST, .handler = apps_action_handler },
        { .uri = "/api/lua-tasks", .method = HTTP_GET, .handler = tasks_list_handler },
        { .uri = "/api/lua-tasks/*", .method = HTTP_POST, .handler = tasks_action_handler },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
