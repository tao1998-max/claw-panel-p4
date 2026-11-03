/*
 * Runtime API for the MCP bridge and the Claw page.
 */
#include "claw_api.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "sdkconfig.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "cap_lua.h"
#include "cap_lua_internal.h"
#include "cap_scheduler.h"
#include "claw_agent.h"
#include "claw_cap.h"
#include "claw_paths.h"
#include "claw_port.h"
#include "claw_settings.h"
#include "claw_storage.h"
#include "claw_tasks.h"

static const char *TAG = "claw_api";

#define CLAW_API_TEMP_PREFIX "/scripts/_ai_"
#define CLAW_API_ASYNC_LOG   4096
#define CLAW_API_MAX_TIMEOUT 8000
#define CLAW_TIME_VALID      1600000000LL

static esp_err_t not_ready(char *out, size_t out_size)
{
    snprintf(out, out_size, "Error: Claw is not available (check CONFIG_TEMPLATE_ENABLE_CLAW)");
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t fail_msg(char *out, size_t out_size, const char *message)
{
    snprintf(out, out_size, "Error: %s", message ? message : "unknown error");
    return ESP_FAIL;
}

static bool has_lua_suffix(const char *path)
{
    size_t len = path ? strlen(path) : 0;
    return len > 4 && strcmp(path + len - 4, ".lua") == 0;
}

/* Extra absolute roots allowed for start_task(path=...), e.g. SD-card apps.
 * Registered once during claw_port_init() before any task can start, so no
 * lock is needed here. */
#define CLAW_API_EXTRA_ROOTS_MAX 2
#define CLAW_API_ROOT_MAX_LEN    96

static char s_extra_roots[CLAW_API_EXTRA_ROOTS_MAX][CLAW_API_ROOT_MAX_LEN];
static size_t s_extra_root_count;

esp_err_t claw_api_add_script_root(const char *root)
{
    size_t len;

    if (!root || root[0] != '/' || strstr(root, "..") != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    len = strlen(root);
    while (len > 1 && root[len - 1] == '/') {
        len--;
    }
    if (len < 2 || len >= CLAW_API_ROOT_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < s_extra_root_count; i++) {
        if (strncmp(s_extra_roots[i], root, len) == 0 && s_extra_roots[i][len] == '\0') {
            return ESP_OK;
        }
    }
    if (s_extra_root_count >= CLAW_API_EXTRA_ROOTS_MAX) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(s_extra_roots[s_extra_root_count], root, len);
    s_extra_roots[s_extra_root_count][len] = '\0';
    s_extra_root_count++;
    return ESP_OK;
}

static bool under_root(const char *path, const char *root)
{
    size_t root_len = root ? strlen(root) : 0;
    return root_len > 1 && strncmp(path, root, root_len) == 0 && path[root_len] == '/';
}

static bool resolve_script_path(const char *input, char *out, size_t out_size)
{
    if (!input || !input[0] || strstr(input, "..") != NULL) {
        return false;
    }
    if (input[0] == '/') {
        const char *data = claw_paths_get(CLAW_PATH_DATA);
        const char *system = claw_paths_get(CLAW_PATH_SYSTEM);
        bool allowed = (data && strncmp(input, data, strlen(data)) == 0) ||
                       (system && strncmp(input, system, strlen(system)) == 0);
        for (size_t i = 0; !allowed && i < s_extra_root_count; i++) {
            allowed = under_root(input, s_extra_roots[i]);
        }
        if (!allowed || !has_lua_suffix(input)) {
            return false;
        }
        strlcpy(out, input, out_size);
        return true;
    }

    const char *data = claw_paths_get(CLAW_PATH_DATA);
    if (!data) {
        return false;
    }
    int written = snprintf(out, out_size, "%s/%s", data, input);
    return written > 0 && (size_t)written < out_size && has_lua_suffix(out);
}

static const char *args_or_null(const char *args_json, char *copy, size_t copy_size)
{
    if (!args_json || !args_json[0]) {
        return NULL;
    }
    cJSON *parsed = cJSON_Parse(args_json);
    if (!parsed) {
        return NULL;
    }
    bool is_object = cJSON_IsObject(parsed);
    cJSON_Delete(parsed);
    if (!is_object || strlen(args_json) >= copy_size) {
        return NULL;
    }
    strlcpy(copy, args_json, copy_size);
    return copy;
}

static esp_err_t write_temp_script(const char *code, char *path, size_t path_size,
                                   char *out, size_t out_size)
{
    int written = snprintf(path, path_size, CLAW_STORAGE_DATA_DIR CLAW_API_TEMP_PREFIX "%08x.lua",
                           (unsigned)esp_random());
    if (written <= 0 || (size_t)written >= path_size) {
        return fail_msg(out, out_size, "script path too long");
    }

    FILE *file = fopen(path, "w");
    if (!file) {
        return fail_msg(out, out_size, "open script failed");
    }
    size_t len = strlen(code);
    size_t stored = fwrite(code, 1, len, file);
    fflush(file);
    fsync(fileno(file));
    fclose(file);
    if (stored != len) {
        unlink(path);
        return fail_msg(out, out_size, "short write");
    }
    return ESP_OK;
}

static void job_event_cb(const cap_lua_job_event_t *event, void *ctx)
{
    (void)ctx;
    if (!event || event->type != CAP_LUA_JOB_EVENT_TERMINAL) {
        return;
    }
    {
        char info[512] = {0};
        if (cap_lua_get_job(event->job_id, info, sizeof(info)) == ESP_OK) {
            ESP_LOGD(TAG, "job %s terminal: %s", event->job_id, info);
        }
    }

    /* AI-generated scripts are temporary; their output lives in the job log. */
    if (strstr(event->path, CLAW_API_TEMP_PREFIX) != NULL) {
        unlink(event->path);
    }
}

esp_err_t claw_api_init(void)
{
    esp_err_t err = cap_lua_register_job_event_cb(job_event_cb, NULL);
    if (err != ESP_OK) {
        return err;
    }
    return claw_task_registry_init();
}

esp_err_t claw_api_status_json(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_port_is_ready()) {
        snprintf(out, out_size, "{\"ready\":false}");
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t total = 0;
    uint64_t free_bytes = 0;
    (void)claw_storage_get_space(&total, &free_bytes);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ready", true);
    cJSON_AddBoolToObject(root, "lua_ai", claw_settings_lua_ai_enabled());
    bool agent_enabled = claw_settings_agent_enabled();
    cJSON_AddBoolToObject(root, "agent", agent_enabled);
    cJSON_AddBoolToObject(root, "agent_configured",
                          agent_enabled && claw_settings_agent_configured());
    cJSON_AddBoolToObject(root, "agent_ready", claw_agent_is_ready());
    cJSON_AddBoolToObject(root, "agent_busy", claw_agent_is_busy());
    cJSON_AddNumberToObject(root, "async_jobs", (double)cap_lua_get_active_async_job_count());
    cJSON_AddNumberToObject(root, "storage_total_kb", (double)(total / 1024));
    cJSON_AddNumberToObject(root, "storage_free_kb", (double)(free_bytes / 1024));
    cJSON_AddBoolToObject(root, "time_valid", time(NULL) > (time_t)CLAW_TIME_VALID);

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(out, text, out_size);
    cJSON_free(text);
    return ESP_OK;
}

esp_err_t claw_api_run_lua(const char *code, const char *args_json,
                           uint32_t timeout_ms, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_port_is_ready()) {
        return not_ready(out, out_size);
    }
    if (!code || !code[0]) {
        return fail_msg(out, out_size, "empty code");
    }
    size_t code_len = strlen(code);
#ifdef CONFIG_TEMPLATE_CLAW_MCP_MAX_SCRIPT
    if (code_len > CONFIG_TEMPLATE_CLAW_MCP_MAX_SCRIPT) {
        return fail_msg(out, out_size, "script too large");
    }
#endif

    if (timeout_ms == 0) {
#ifdef CONFIG_TEMPLATE_CLAW_LUA_SYNC_MS
        timeout_ms = CONFIG_TEMPLATE_CLAW_LUA_SYNC_MS;
#else
        timeout_ms = 3000;
#endif
    }
    if (timeout_ms > CLAW_API_MAX_TIMEOUT) {
        timeout_ms = CLAW_API_MAX_TIMEOUT;
    }

    char path[192];
    esp_err_t err = write_temp_script(code, path, sizeof(path), out, out_size);
    if (err != ESP_OK) {
        return err;
    }

    err = cap_lua_run_script(path, args_json, timeout_ms, out, out_size);
    unlink(path);
    return err;
}

esp_err_t claw_api_start_task_ex(const char *name, const char *code, const char *path,
                                 const char *args_json, uint32_t timeout_ms,
                                 claw_task_origin_t origin,
                                 char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_port_is_ready()) {
        return not_ready(out, out_size);
    }

    /* Quotas keep the voice assistant from flooding the job table:
     * AI <= 2, agent <= 1, one concurrency slot stays free for the user. */
    if (origin == CLAW_TASK_ORIGIN_AI && claw_task_active_count(origin) >= 2) {
        return fail_msg(out, out_size,
                        "too many AI tasks running (max 2); stop one with stop_task");
    }
    if (origin == CLAW_TASK_ORIGIN_AGENT && claw_task_active_count(origin) >= 1) {
        return fail_msg(out, out_size,
                        "an agent task is already running; wait or stop it first");
    }

    char resolved[CAP_LUA_JOB_PATH_MAX];
    bool temp_script = false;

    if (code && code[0]) {
#ifdef CONFIG_TEMPLATE_CLAW_MCP_MAX_SCRIPT
        if (strlen(code) > CONFIG_TEMPLATE_CLAW_MCP_MAX_SCRIPT) {
            return fail_msg(out, out_size, "script too large");
        }
#endif
        esp_err_t write_err = write_temp_script(code, resolved, sizeof(resolved), out, out_size);
        if (write_err != ESP_OK) {
            return write_err;
        }
        temp_script = true;
    } else if (path && path[0]) {
        if (!resolve_script_path(path, resolved, sizeof(resolved))) {
            return fail_msg(out, out_size, "path must be a .lua script under the Claw workspace");
        }
    } else {
        return fail_msg(out, out_size, "provide code or path");
    }

    char args_copy[512];
    const char *args = args_or_null(args_json, args_copy, sizeof(args_copy));

    cap_lua_async_job_t job = {0};
    strlcpy(job.path, resolved, sizeof(job.path));
    if (name && name[0]) {
        strlcpy(job.name, name, sizeof(job.name));
    }
    job.args_json = (char *)args;
    job.timeout_ms = timeout_ms;
    job.log_bytes = CLAW_API_ASYNC_LOG;

    char job_id[CAP_LUA_JOB_ID_LEN] = {0};
    char err_buf[256] = {0};
    esp_err_t err = cap_lua_async_submit(&job, job_id, sizeof(job_id),
                                         err_buf, sizeof(err_buf));
    if (err != ESP_OK) {
        if (temp_script) {
            unlink(resolved);
        }
        snprintf(out, out_size, "Error: %s", err_buf[0] ? err_buf : esp_err_to_name(err));
        return err;
    }

    claw_task_note_submit(job_id, job.name, resolved, origin);

    snprintf(out, out_size,
             "{\"job_id\":\"%s\",\"name\":\"%s\",\"status\":\"queued\"}",
             job_id, job.name[0] ? job.name : "(unnamed)");
    ESP_LOGI(TAG, "started task %s origin=%s path=%s", job_id,
             claw_task_origin_name(origin), resolved);
    return ESP_OK;
}

esp_err_t claw_api_start_task(const char *name, const char *code, const char *path,
                              const char *args_json, uint32_t timeout_ms,
                              char *out, size_t out_size)
{
    return claw_api_start_task_ex(name, code, path, args_json, timeout_ms,
                                  CLAW_TASK_ORIGIN_USER, out, out_size);
}

esp_err_t claw_api_list_tasks(const char *state, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_port_is_ready()) {
        return not_ready(out, out_size);
    }
    return cap_lua_list_jobs(state && state[0] ? state : NULL, out, out_size);
}

esp_err_t claw_api_get_task(const char *id, char *out, size_t out_size)
{
    if (!out || out_size == 0 || !id || !id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_port_is_ready()) {
        return not_ready(out, out_size);
    }
    if (claw_agent_is_agent_task_id(id)) {
        return claw_agent_get_task_output(id, out, out_size);
    }

    char base[384] = {0};
    esp_err_t err = cap_lua_get_job(id, base, sizeof(base));
    if (err != ESP_OK) {
        strlcpy(out, base[0] ? base : "Error: job not found", out_size);
        return err;
    }

    int percent = 0;
    char message[96] = {0};
    if (claw_task_get_progress(id, &percent, message, sizeof(message)) == ESP_OK) {
        snprintf(out, out_size, "%sprogress=%d%% %s\n", base, percent, message);
    } else {
        strlcpy(out, base, out_size);
    }
    return ESP_OK;
}

esp_err_t claw_api_stop_task(const char *id, uint32_t wait_ms, char *out, size_t out_size)
{
    if (!out || out_size == 0 || !id || !id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_port_is_ready()) {
        return not_ready(out, out_size);
    }
    if (wait_ms > 5000) {
        wait_ms = 5000;
    }
    if (claw_agent_is_agent_task_id(id)) {
        return claw_agent_stop_task(id, wait_ms, out, out_size);
    }
    return cap_lua_stop_job(id, wait_ms, out, out_size);
}

static int64_t parse_positive(const char *text)
{
    if (!text || !text[0]) {
        return -1;
    }
    char *end = NULL;
    long long value = strtoll(text, &end, 10);
    if (end == text || (end && *end != '\0') || value <= 0) {
        return -1;
    }
    return (int64_t)value;
}

esp_err_t claw_api_add_schedule(const char *id, const char *kind, const char *when,
                                const char *script, const char *args_json,
                                char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_port_is_ready()) {
        return not_ready(out, out_size);
    }
    if (!id || !id[0] || !kind || !when || !when[0] || !script || !script[0]) {
        return fail_msg(out, out_size, "id, kind, when and script are required");
    }

    char resolved[CAP_LUA_JOB_PATH_MAX];
    if (!resolve_script_path(script, resolved, sizeof(resolved))) {
        return fail_msg(out, out_size, "script must be a .lua file under the Claw workspace");
    }

    cJSON *args = NULL;
    if (args_json && args_json[0]) {
        args = cJSON_Parse(args_json);
        if (!cJSON_IsObject(args)) {
            cJSON_Delete(args);
            return fail_msg(out, out_size, "args must be a JSON object");
        }
    }
    if (args == NULL) {
        args = cJSON_CreateObject();
        if (args == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    cJSON *schedule = cJSON_CreateObject();
    cJSON *rule = cJSON_CreateObject();
    cJSON *match = cJSON_CreateObject();
    cJSON *actions = cJSON_CreateArray();
    cJSON *action = cJSON_CreateObject();
    cJSON *input = cJSON_CreateObject();
    if (schedule == NULL || rule == NULL || match == NULL ||
        actions == NULL || action == NULL || input == NULL) {
        cJSON_Delete(schedule);
        cJSON_Delete(rule);
        cJSON_Delete(match);
        cJSON_Delete(actions);
        cJSON_Delete(action);
        cJSON_Delete(input);
        cJSON_Delete(args);
        return ESP_ERR_NO_MEM;
    }

    int64_t start_at_ms = 0;
    int64_t interval_ms = 0;
    const char *cron_expr = "";
    int max_runs = 0;
    if (strcmp(kind, "once") == 0) {
        int64_t value = parse_positive(when);
        if (value < 0) {
            cJSON_Delete(schedule);
            cJSON_Delete(rule);
            cJSON_Delete(match);
            cJSON_Delete(actions);
            cJSON_Delete(action);
            cJSON_Delete(input);
            cJSON_Delete(args);
            return fail_msg(out, out_size,
                            "once expects epoch seconds (or seconds from now)");
        }
        struct timeval tv = {0};
        gettimeofday(&tv, NULL);
        int64_t now_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
        start_at_ms = value >= CLAW_TIME_VALID ? value * 1000 : now_ms + value * 1000;
        max_runs = 1;
    } else if (strcmp(kind, "interval") == 0) {
        int64_t value = parse_positive(when);
        if (value < 0) {
            cJSON_Delete(schedule);
            cJSON_Delete(rule);
            cJSON_Delete(match);
            cJSON_Delete(actions);
            cJSON_Delete(action);
            cJSON_Delete(input);
            cJSON_Delete(args);
            return fail_msg(out, out_size, "interval expects positive seconds");
        }
        interval_ms = value * 1000;
    } else if (strcmp(kind, "cron") == 0) {
        cron_expr = when;
    } else {
        cJSON_Delete(schedule);
        cJSON_Delete(rule);
        cJSON_Delete(match);
        cJSON_Delete(actions);
        cJSON_Delete(action);
        cJSON_Delete(input);
        cJSON_Delete(args);
        return fail_msg(out, out_size, "kind must be once, interval or cron");
    }

    /* Same schedule shape as the official scheduled_task skill. */
    cJSON_AddStringToObject(schedule, "id", id);
    cJSON_AddBoolToObject(schedule, "enabled", true);
    cJSON_AddStringToObject(schedule, "kind", kind);
    cJSON_AddNumberToObject(schedule, "start_at_ms", (double)start_at_ms);
    cJSON_AddNumberToObject(schedule, "end_at_ms", 0);
    cJSON_AddNumberToObject(schedule, "interval_ms", (double)interval_ms);
    cJSON_AddStringToObject(schedule, "cron_expr", cron_expr);
    cJSON_AddStringToObject(schedule, "event_type", "schedule");
    cJSON_AddStringToObject(schedule, "event_key", id);
    cJSON_AddStringToObject(schedule, "source_channel", "time");
    cJSON_AddStringToObject(schedule, "chat_id", "");
    cJSON_AddStringToObject(schedule, "content_type", "trigger");
    cJSON_AddStringToObject(schedule, "session_policy", "trigger");
    cJSON_AddStringToObject(schedule, "text", id);
    cJSON_AddStringToObject(schedule, "payload_json", "{}");
    cJSON_AddNumberToObject(schedule, "max_runs", max_runs);

    char rule_id[CAP_LUA_JOB_ID_LEN + 8];
    snprintf(rule_id, sizeof(rule_id), "%s_run", id);
    cJSON_AddStringToObject(rule, "id", rule_id);
    cJSON_AddStringToObject(rule, "description", "Run a Lua script for a scheduled task");
    cJSON_AddBoolToObject(rule, "enabled", true);
    cJSON_AddBoolToObject(rule, "consume_on_match", false);
    cJSON_AddStringToObject(match, "event_type", "schedule");
    cJSON_AddStringToObject(match, "event_key", id);
    cJSON_AddItemToObject(rule, "match", match);
    cJSON_AddStringToObject(action, "type", "run_script");
    cJSON_AddStringToObject(input, "path", resolved);
    cJSON_AddItemToObject(input, "args", args);
    cJSON_AddItemToObject(action, "input", input);
    cJSON_AddItemToArray(actions, action);
    cJSON_AddItemToObject(rule, "actions", actions);

    char *schedule_text = cJSON_PrintUnformatted(schedule);
    char *rule_text = cJSON_PrintUnformatted(rule);
    cJSON_Delete(schedule);
    cJSON_Delete(rule);
    if (schedule_text == NULL || rule_text == NULL) {
        cJSON_free(schedule_text);
        cJSON_free(rule_text);
        return ESP_ERR_NO_MEM;
    }

    claw_cap_call_context_t ctx = {
        .caller = CLAW_CAP_CALLER_SYSTEM,
        .source_cap = "claw_api",
    };
    char cap_out[1024] = {0};

    cJSON *request = cJSON_CreateObject();
    if (request == NULL) {
        cJSON_free(schedule_text);
        cJSON_free(rule_text);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(request, "rule_json", rule_text);
    char *request_text = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (request_text == NULL) {
        cJSON_free(schedule_text);
        cJSON_free(rule_text);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = claw_cap_call("add_router_rule", request_text, &ctx,
                                  cap_out, sizeof(cap_out));
    cJSON_free(request_text);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "add_router_rule failed: %s %s", esp_err_to_name(err), cap_out);
        cJSON_free(schedule_text);
        cJSON_free(rule_text);
        snprintf(out, out_size, "Error: add router rule failed (%s)",
                 esp_err_to_name(err));
        return err;
    }

    request = cJSON_CreateObject();
    if (request == NULL) {
        cJSON_free(schedule_text);
        cJSON_free(rule_text);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(request, "schedule_json", schedule_text);
    request_text = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (request_text == NULL) {
        cJSON_free(schedule_text);
        cJSON_free(rule_text);
        return ESP_ERR_NO_MEM;
    }
    cap_out[0] = '\0';
    err = claw_cap_call("scheduler_add", request_text, &ctx,
                        cap_out, sizeof(cap_out));
    cJSON_free(request_text);
    cJSON_free(schedule_text);
    cJSON_free(rule_text);
    if (err != ESP_OK) {
        /* Roll back the rule, same as the official scheduled_task skill. */
        cJSON *rollback = cJSON_CreateObject();
        if (rollback != NULL) {
            cJSON_AddStringToObject(rollback, "id", rule_id);
            char *rollback_text = cJSON_PrintUnformatted(rollback);
            cJSON_Delete(rollback);
            if (rollback_text != NULL) {
                char rollback_out[256] = {0};
                (void)claw_cap_call("delete_router_rule", rollback_text, &ctx,
                                    rollback_out, sizeof(rollback_out));
                cJSON_free(rollback_text);
            }
        }
        ESP_LOGW(TAG, "scheduler_add failed: %s %s", esp_err_to_name(err), cap_out);
        snprintf(out, out_size, "Error: add schedule failed (%s)",
                 esp_err_to_name(err));
        return err;
    }

    snprintf(out, out_size,
             "{\"id\":\"%s\",\"kind\":\"%s\",\"router_rule\":\"%s\",\"status\":\"scheduled\"}",
             id, kind, rule_id);
    return ESP_OK;
}

esp_err_t claw_api_list_scripts_json(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_port_is_ready()) {
        return not_ready(out, out_size);
    }

    const char *data = claw_paths_get(CLAW_PATH_DATA);
    if (!data) {
        return fail_msg(out, out_size, "workspace unavailable");
    }

    char dir_path[192];
    int written = snprintf(dir_path, sizeof(dir_path), "%s/scripts", data);
    if (written <= 0 || (size_t)written >= sizeof(dir_path)) {
        return fail_msg(out, out_size, "workspace path too long");
    }

    cJSON *array = cJSON_CreateArray();
    DIR *dir = opendir(dir_path);
    if (array && dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            size_t len = strlen(entry->d_name);
            if (len <= 4 || strcmp(entry->d_name + len - 4, ".lua") != 0 ||
                entry->d_name[0] == '_') {
                continue;
            }
            char full[512];
            struct stat st;
            int full_len = snprintf(full, sizeof(full), "%s/%s", dir_path, entry->d_name);
            if (full_len <= 0 || (size_t)full_len >= sizeof(full)) {
                continue;
            }
            cJSON *item = cJSON_CreateObject();
            if (!item) {
                continue;
            }
            cJSON_AddStringToObject(item, "name", entry->d_name);
            cJSON_AddStringToObject(item, "path", entry->d_name);
            if (stat(full, &st) == 0) {
                cJSON_AddNumberToObject(item, "size", (double)st.st_size);
            }
            cJSON_AddItemToArray(array, item);
        }
    }
    if (dir) {
        closedir(dir);
    }
    if (!array) {
        return ESP_ERR_NO_MEM;
    }

    char *text = cJSON_PrintUnformatted(array);
    cJSON_Delete(array);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(out, text, out_size);
    cJSON_free(text);
    return ESP_OK;
}

bool claw_api_is_script_active(const char *path)
{
    if (!claw_port_is_ready() || path == NULL || path[0] == '\0') {
        return false;
    }

    char resolved[CAP_LUA_JOB_PATH_MAX];
    if (!resolve_script_path(path, resolved, sizeof(resolved))) {
        return false;
    }

    char snapshot[4096] = {0};
    if (claw_task_registry_snapshot_json(snapshot, sizeof(snapshot)) != ESP_OK) {
        return false;
    }
    cJSON *array = cJSON_Parse(snapshot);
    if (!cJSON_IsArray(array)) {
        cJSON_Delete(array);
        return false;
    }

    bool active = false;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        const char *state = cJSON_GetStringValue(cJSON_GetObjectItem(item, "state"));
        const char *entry_path = cJSON_GetStringValue(cJSON_GetObjectItem(item, "path"));
        if (state != NULL && entry_path != NULL &&
            (strcmp(state, "queued") == 0 || strcmp(state, "running") == 0) &&
            strcmp(entry_path, resolved) == 0) {
            active = true;
            break;
        }
    }
    cJSON_Delete(array);
    return active;
}

/* ---------- skills (SKILL.md directories) ---------- */

/* Copy the JSON object between the first and second "---" lines. */
static bool read_frontmatter_json(const char *path, char *out, size_t out_size)
{
    if (out == NULL || out_size < 8) {
        return false;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }
    size_t n = fread(out, 1, out_size - 1, f);
    fclose(f);
    out[n] = '\0';

    char *start = out;
    if (strncmp(start, "---", 3) != 0) {
        return false;
    }
    start = strchr(start, '\n');
    if (start == NULL) {
        return false;
    }
    start++;
    char *end = strstr(start, "\n---");
    if (end == NULL) {
        return false;
    }
    *end = '\0';
    memmove(out, start, strlen(start) + 1);
    return true;
}

static bool array_has_id(cJSON *array, const char *id)
{
    if (array == NULL || id == NULL || id[0] == '\0') {
        return false;
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        const char *existing = cJSON_GetStringValue(cJSON_GetObjectItem(item, "id"));
        if (existing != NULL && strcmp(existing, id) == 0) {
            return true;
        }
    }
    return false;
}

/* Append <root>/skills/<id>/SKILL.md entries. The first .lua under the skill's
 * scripts/ directory is the runnable entry point. User skills shadow system
 * skills with the same id. */
static void skills_append_root(cJSON *array, const char *root, bool user)
{
    if (array == NULL || root == NULL) {
        return;
    }

    char skills_dir[256];
    int n = snprintf(skills_dir, sizeof(skills_dir), "%s/skills", root);
    if (n <= 0 || (size_t)n >= sizeof(skills_dir)) {
        return;
    }
    DIR *dir = opendir(skills_dir);
    if (dir == NULL) {
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (array_has_id(array, entry->d_name)) {
            continue;
        }

        char skill_md[640];
        struct stat st;
        n = snprintf(skill_md, sizeof(skill_md), "%s/%s/SKILL.md",
                     skills_dir, entry->d_name);
        if (n <= 0 || (size_t)n >= sizeof(skill_md) || stat(skill_md, &st) != 0) {
            continue;
        }

        char frontmatter[2048];
        if (!read_frontmatter_json(skill_md, frontmatter, sizeof(frontmatter))) {
            continue;
        }
        cJSON *meta = cJSON_Parse(frontmatter);
        if (!cJSON_IsObject(meta)) {
            cJSON_Delete(meta);
            continue;
        }

        const char *json_name = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "name"));
        const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "title"));
        const char *description = cJSON_GetStringValue(cJSON_GetObjectItem(meta, "description"));
        if (title == NULL || title[0] == '\0') {
            title = json_name != NULL && json_name[0] ? json_name : entry->d_name;
        }
        if (description == NULL) {
            description = "";
        }

        char scripts_dir[640];
        n = snprintf(scripts_dir, sizeof(scripts_dir), "%s/%s/scripts",
                     skills_dir, entry->d_name);
        DIR *scripts = (n > 0 && (size_t)n < sizeof(scripts_dir))
                       ? opendir(scripts_dir) : NULL;
        if (scripts == NULL) {
            cJSON_Delete(meta);
            continue;
        }

        char script[768] = {0};
        struct dirent *candidate;
        while ((candidate = readdir(scripts)) != NULL) {
            size_t len = strlen(candidate->d_name);
            if (len > 4 && strcmp(candidate->d_name + len - 4, ".lua") == 0) {
                snprintf(script, sizeof(script), "%s/%s",
                         scripts_dir, candidate->d_name);
                break;
            }
        }
        closedir(scripts);
        if (script[0] == '\0') {
            cJSON_Delete(meta);
            continue;
        }

        cJSON *item = cJSON_CreateObject();
        if (item == NULL) {
            cJSON_Delete(meta);
            continue;
        }
        cJSON_AddStringToObject(item, "id", entry->d_name);
        cJSON_AddStringToObject(item, "title", title);
        cJSON_AddStringToObject(item, "description", description);
        cJSON_AddStringToObject(item, "path", script);
        cJSON_AddStringToObject(item, "source", user ? "user" : "system");
        cJSON_AddItemToArray(array, item);
        cJSON_Delete(meta);
    }
    closedir(dir);
}

esp_err_t claw_api_list_skills_json(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_port_is_ready()) {
        return not_ready(out, out_size);
    }

    cJSON *array = cJSON_CreateArray();
    if (!array) {
        return ESP_ERR_NO_MEM;
    }
    skills_append_root(array, claw_paths_get(CLAW_PATH_DATA), true);
    skills_append_root(array, claw_paths_get(CLAW_PATH_SYSTEM), false);

    char *text = cJSON_PrintUnformatted(array);
    cJSON_Delete(array);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(out, text, out_size);
    cJSON_free(text);
    return ESP_OK;
}

esp_err_t claw_api_list_schedules(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_port_is_ready()) {
        return not_ready(out, out_size);
    }
    esp_err_t err = cap_scheduler_list_json(out, out_size);
    if (err != ESP_OK) {
        snprintf(out, out_size, "Error: list schedules failed (%s)", esp_err_to_name(err));
    }
    return err;
}

static void schedule_delete_rule(const char *rule_id)
{
    cJSON *request = cJSON_CreateObject();
    if (request == NULL) {
        return;
    }
    cJSON_AddStringToObject(request, "id", rule_id);
    char *text = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (text == NULL) {
        return;
    }
    claw_cap_call_context_t ctx = {
        .caller = CLAW_CAP_CALLER_SYSTEM,
        .source_cap = "claw_api",
    };
    char ignored[256] = {0};
    (void)claw_cap_call("delete_router_rule", text, &ctx,
                        ignored, sizeof(ignored));
    cJSON_free(text);
}

esp_err_t claw_api_remove_schedule(const char *id, char *out, size_t out_size)
{
    if (!out || out_size == 0 || !id || !id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_port_is_ready()) {
        return not_ready(out, out_size);
    }
    esp_err_t err = cap_scheduler_remove(id);
    if (err == ESP_OK) {
        char rule_id[CAP_LUA_JOB_ID_LEN + 16];
        snprintf(rule_id, sizeof(rule_id), "%s_run", id);
        schedule_delete_rule(rule_id);
        snprintf(rule_id, sizeof(rule_id), "%s_notify", id);
        schedule_delete_rule(rule_id);
        snprintf(out, out_size, "Removed schedule %s", id);
    } else {
        snprintf(out, out_size, "Error: remove failed (%s)", esp_err_to_name(err));
    }
    return err;
}

esp_err_t claw_api_set_schedule_enabled(const char *id, bool enabled,
                                        char *out, size_t out_size)
{
    if (!out || out_size == 0 || !id || !id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_port_is_ready()) {
        return not_ready(out, out_size);
    }
    esp_err_t err = cap_scheduler_enable(id, enabled);
    if (err == ESP_OK) {
        snprintf(out, out_size, "Schedule %s %s", id, enabled ? "enabled" : "disabled");
    } else {
        snprintf(out, out_size, "Error: update failed (%s)", esp_err_to_name(err));
    }
    return err;
}

static bool script_name_ok(const char *name, char *out, size_t out_size)
{
    if (name == NULL || name[0] == '\0' || strlen(name) > 64 || name[0] == '_') {
        return false;
    }
    for (const char *p = name; *p != '\0'; p++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) {
            return false;
        }
    }
    if (strstr(name, "..") != NULL || strchr(name, '/') != NULL ||
        strchr(name, '\\') != NULL) {
        return false;
    }
    size_t len = strlen(name);
    if (len > 4 && strcmp(name + len - 4, ".lua") == 0) {
        if (len >= out_size) {
            return false;
        }
        strlcpy(out, name, out_size);
    } else {
        int n = snprintf(out, out_size, "%s.lua", name);
        if (n <= 0 || (size_t)n >= out_size) {
            return false;
        }
    }
    return true;
}

esp_err_t claw_api_remove_task(const char *id, char *out, size_t out_size)
{
    if (!out || out_size == 0 || !id || !id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_port_is_ready()) {
        return not_ready(out, out_size);
    }
    if (!claw_task_remove(id)) {
        return fail_msg(out, out_size, "task not found or still running");
    }
    snprintf(out, out_size, "OK: task %s removed", id);
    return ESP_OK;
}

esp_err_t claw_api_clear_finished_tasks(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_port_is_ready()) {
        return not_ready(out, out_size);
    }
    size_t count = claw_task_clear_finished();
    snprintf(out, out_size, "OK: %u finished task(s) removed", (unsigned)count);
    return ESP_OK;
}

esp_err_t claw_api_create_script(const char *name, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_port_is_ready()) {
        return not_ready(out, out_size);
    }
    char file_name[80];
    if (!script_name_ok(name, file_name, sizeof(file_name))) {
        return fail_msg(out, out_size,
                        "name must be 1-64 chars of a-z/A-Z/0-9/_/- and end with .lua");
    }
    const char *data = claw_paths_get(CLAW_PATH_DATA);
    if (data == NULL) {
        return fail_msg(out, out_size, "workspace unavailable");
    }
    char path[320];
    int n = snprintf(path, sizeof(path), "%s/scripts/%s", data, file_name);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        return fail_msg(out, out_size, "script path too long");
    }
    struct stat st;
    if (stat(path, &st) == 0) {
        return fail_msg(out, out_size, "script already exists");
    }

    char content[256];
    n = snprintf(content, sizeof(content),
                 "-- %s: created on the panel\n"
                 "print(\"hello from %s\")\n", file_name, file_name);
    if (n <= 0 || (size_t)n >= sizeof(content)) {
        return fail_msg(out, out_size, "name too long");
    }
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return fail_msg(out, out_size, "open script failed");
    }
    bool ok = fwrite(content, 1, (size_t)n, file) == (size_t)n;
    fclose(file);
    if (!ok) {
        unlink(path);
        return fail_msg(out, out_size, "write script failed");
    }
    snprintf(out, out_size, "{\"name\":\"%s\",\"path\":\"scripts/%s\"}",
             file_name, file_name);
    return ESP_OK;
}

esp_err_t claw_api_delete_script(const char *name, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_port_is_ready()) {
        return not_ready(out, out_size);
    }
    char file_name[80];
    if (!script_name_ok(name, file_name, sizeof(file_name))) {
        return fail_msg(out, out_size, "invalid script name");
    }
    const char *data = claw_paths_get(CLAW_PATH_DATA);
    if (data == NULL) {
        return fail_msg(out, out_size, "workspace unavailable");
    }
    char path[320];
    int n = snprintf(path, sizeof(path), "%s/scripts/%s", data, file_name);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        return fail_msg(out, out_size, "script path too long");
    }
    if (unlink(path) != 0) {
        return fail_msg(out, out_size, errno == ENOENT ? "script not found" : "delete failed");
    }
    snprintf(out, out_size, "OK: deleted %s", file_name);
    return ESP_OK;
}

/* ---------- local agent ---------- */

esp_err_t claw_api_agent_status_json(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_port_is_ready()) {
        return not_ready(out, out_size);
    }
    return claw_agent_status_json(out, out_size);
}

esp_err_t claw_api_agent_stream_snapshot(const char *session_id, char *out,
                                         size_t out_size, uint32_t *out_rev,
                                         bool *out_active)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (!claw_port_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    return claw_agent_stream_snapshot(session_id, out, out_size, out_rev,
                                      out_active);
}

esp_err_t claw_api_agent_last_reply(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_port_is_ready()) {
        return not_ready(out, out_size);
    }
    return claw_agent_last_reply(out, out_size);
}

esp_err_t claw_api_agent_set_enabled(bool enabled, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_port_is_ready()) {
        return not_ready(out, out_size);
    }
    if (enabled && !claw_settings_agent_configured()) {
        snprintf(out, out_size, "Error: 请先填写接口地址和模型名称");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = claw_settings_set_agent(enabled);
    if (err != ESP_OK) {
        snprintf(out, out_size, "Error: 保存开关失败 (%s)", esp_err_to_name(err));
        return err;
    }
    claw_agent_settings_changed();
    snprintf(out, out_size, "OK: agent %s", enabled ? "enabled" : "disabled");
    return ESP_OK;
}

void claw_api_agent_settings_changed(void)
{
    claw_agent_settings_changed();
}

esp_err_t claw_api_agent_ask(const char *prompt, const char *origin,
                             uint32_t timeout_ms, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_port_is_ready()) {
        return not_ready(out, out_size);
    }
    return claw_agent_ask(prompt, origin, timeout_ms, out, out_size);
}

esp_err_t claw_api_agent_start_task(const char *name, const char *prompt,
                                    uint32_t timeout_ms, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_port_is_ready()) {
        return not_ready(out, out_size);
    }
    return claw_agent_start_task(name, prompt, timeout_ms, out, out_size);
}
