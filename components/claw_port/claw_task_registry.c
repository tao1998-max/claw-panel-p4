/*
 * Claw task registry.
 *
 * cap_lua already tracks the live Lua jobs; this table adds what the product
 * needs: who started each job (AI / user / schedule / agent), start/end times
 * and the latest progress. It is a bounded ring: finished entries are evicted
 * oldest-first when a new job arrives.
 */
#include "claw_tasks.h"

#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "cap_lua.h"
#include "cap_lua_internal.h"

static const char *TAG = "claw_tasks";

#define CLAW_TASK_SLOTS      16
#define CLAW_TASK_NAME_LEN   32
#define CLAW_TASK_PATH_LEN   128
#define CLAW_TASK_MSG_LEN    64

typedef struct {
    bool used;
    char job_id[16];
    char name[CLAW_TASK_NAME_LEN];
    char path[CLAW_TASK_PATH_LEN];
    uint8_t origin;
    uint8_t status;
    int progress;
    char message[CLAW_TASK_MSG_LEN];
    int64_t started_ms;
    int64_t ended_ms;
} task_entry_t;

static task_entry_t s_tasks[CLAW_TASK_SLOTS];
static SemaphoreHandle_t s_lock;
static bool s_init_done;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool status_is_active(uint8_t status)
{
    return status == CAP_LUA_JOB_QUEUED || status == CAP_LUA_JOB_RUNNING;
}

static void lock_tasks(void)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void unlock_tasks(void)
{
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
}

static task_entry_t *find_locked(const char *job_id)
{
    if (!job_id || !job_id[0]) {
        return NULL;
    }
    for (size_t i = 0; i < CLAW_TASK_SLOTS; i++) {
        if (s_tasks[i].used && strcmp(s_tasks[i].job_id, job_id) == 0) {
            return &s_tasks[i];
        }
    }
    return NULL;
}

static task_entry_t *alloc_locked(void)
{
    task_entry_t *oldest_done = NULL;
    for (size_t i = 0; i < CLAW_TASK_SLOTS; i++) {
        if (!s_tasks[i].used) {
            return &s_tasks[i];
        }
        if (!status_is_active(s_tasks[i].status) && !oldest_done) {
            oldest_done = &s_tasks[i];
        }
    }
    /* Ring full of finished jobs: replace the oldest one. */
    if (oldest_done) {
        for (size_t i = 0; i < CLAW_TASK_SLOTS; i++) {
            if (!status_is_active(s_tasks[i].status) &&
                s_tasks[i].ended_ms < oldest_done->ended_ms) {
                oldest_done = &s_tasks[i];
            }
        }
    }
    if (oldest_done) {
        memset(oldest_done, 0, sizeof(*oldest_done));
    }
    return oldest_done;
}

static task_entry_t *get_or_create(const char *job_id, const char *name,
                                   const char *path, claw_task_origin_t origin)
{
    task_entry_t *entry = find_locked(job_id);
    if (entry) {
        return entry;
    }
    entry = alloc_locked();
    if (!entry) {
        return NULL;
    }
    memset(entry, 0, sizeof(*entry));
    entry->used = true;
    entry->origin = (uint8_t)origin;
    entry->status = CAP_LUA_JOB_QUEUED;
    entry->started_ms = now_ms();
    entry->progress = -1;
    strlcpy(entry->job_id, job_id ? job_id : "?", sizeof(entry->job_id));
    if (name) {
        strlcpy(entry->name, name, sizeof(entry->name));
    }
    if (path) {
        strlcpy(entry->path, path, sizeof(entry->path));
    }
    return entry;
}

const char *claw_task_origin_name(claw_task_origin_t origin)
{
    switch (origin) {
    case CLAW_TASK_ORIGIN_AI:       return "ai";
    case CLAW_TASK_ORIGIN_USER:     return "user";
    case CLAW_TASK_ORIGIN_SCHEDULED:return "scheduled";
    case CLAW_TASK_ORIGIN_AGENT:    return "agent";
    default:                        return "unknown";
    }
}

bool claw_task_origin_parse(const char *name, claw_task_origin_t *out)
{
    if (!name || !out) {
        return false;
    }
    for (int i = 0; i < CLAW_TASK_ORIGIN_COUNT; i++) {
        if (strcmp(name, claw_task_origin_name((claw_task_origin_t)i)) == 0) {
            *out = (claw_task_origin_t)i;
            return true;
        }
    }
    return false;
}

static void registry_job_event(const cap_lua_job_event_t *event, void *ctx)
{
    (void)ctx;
    if (!event || !event->job_id[0]) {
        return;
    }

    lock_tasks();
    task_entry_t *entry = find_locked(event->job_id);
    if (!entry && event->type != CAP_LUA_JOB_EVENT_TERMINAL) {
        entry = get_or_create(event->job_id, event->name, event->path,
                              CLAW_TASK_ORIGIN_USER);
    }
    if (entry) {
        if (event->name[0]) {
            strlcpy(entry->name, event->name, sizeof(entry->name));
        }
        switch (event->type) {
        case CAP_LUA_JOB_EVENT_CREATED:
            entry->status = CAP_LUA_JOB_QUEUED;
            break;
        case CAP_LUA_JOB_EVENT_RUNNING:
            entry->status = CAP_LUA_JOB_RUNNING;
            if (entry->started_ms == 0) {
                entry->started_ms = now_ms();
            }
            break;
        case CAP_LUA_JOB_EVENT_TERMINAL:
            entry->status = (uint8_t)event->status;
            entry->ended_ms = now_ms();
            break;
        default:
            break;
        }
    }
    unlock_tasks();
}

esp_err_t claw_task_registry_init(void)
{
    if (s_init_done) {
        return ESP_OK;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    esp_err_t err = cap_lua_register_job_event_cb(registry_job_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register job event cb failed: %s", esp_err_to_name(err));
        return err;
    }
    s_init_done = true;
    return ESP_OK;
}

void claw_task_note_submit(const char *job_id, const char *name, const char *path,
                           claw_task_origin_t origin)
{
    lock_tasks();
    task_entry_t *entry = get_or_create(job_id, name, path, origin);
    if (entry) {
        entry->origin = (uint8_t)origin;
        if (name && name[0]) {
            strlcpy(entry->name, name, sizeof(entry->name));
        }
        if (path && path[0]) {
            strlcpy(entry->path, path, sizeof(entry->path));
        }
    }
    unlock_tasks();
}

void claw_task_set_origin(const char *job_id, claw_task_origin_t origin)
{
    lock_tasks();
    task_entry_t *entry = find_locked(job_id);
    if (entry) {
        entry->origin = (uint8_t)origin;
    }
    unlock_tasks();
}

size_t claw_task_active_count(claw_task_origin_t origin)
{
    size_t count = 0;
    lock_tasks();
    for (size_t i = 0; i < CLAW_TASK_SLOTS; i++) {
        if (s_tasks[i].used && s_tasks[i].origin == (uint8_t)origin &&
            status_is_active(s_tasks[i].status)) {
            count++;
        }
    }
    unlock_tasks();
    return count;
}

void claw_task_report_progress(const char *job_id, int percent, const char *message)
{
    if (!job_id || !job_id[0]) {
        job_id = "?";
    }
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }

    lock_tasks();
    task_entry_t *entry = get_or_create(job_id, NULL, NULL, CLAW_TASK_ORIGIN_USER);
    if (entry) {
        entry->progress = percent;
        strlcpy(entry->message, message ? message : "", sizeof(entry->message));
    }
    unlock_tasks();

    ESP_LOGI(TAG, "job %s: %d%% %s", job_id, percent, message ? message : "");
}

esp_err_t claw_task_get_progress(const char *job_id, int *percent,
                                 char *message, size_t message_size)
{
    if (!percent || !message || message_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_ERR_NOT_FOUND;
    lock_tasks();
    task_entry_t *entry = NULL;
    if (job_id && job_id[0]) {
        entry = find_locked(job_id);
    } else {
        /* No id: newest record with progress. */
        for (size_t i = 0; i < CLAW_TASK_SLOTS; i++) {
            if (s_tasks[i].used && s_tasks[i].progress >= 0 &&
                (!entry || s_tasks[i].started_ms > entry->started_ms)) {
                entry = &s_tasks[i];
            }
        }
    }
    if (entry) {
        *percent = entry->progress;
        strlcpy(message, entry->message, message_size);
        err = ESP_OK;
    }
    unlock_tasks();
    return err;
}

static bool status_from_name(const char *name, uint8_t *out)
{
    if (name == NULL || out == NULL) {
        return false;
    }
    if (strcmp(name, "queued") == 0) {
        *out = CAP_LUA_JOB_QUEUED;
    } else if (strcmp(name, "running") == 0) {
        *out = CAP_LUA_JOB_RUNNING;
    } else if (strcmp(name, "done") == 0) {
        *out = CAP_LUA_JOB_DONE;
    } else if (strcmp(name, "failed") == 0) {
        *out = CAP_LUA_JOB_FAILED;
    } else if (strcmp(name, "timeout") == 0) {
        *out = CAP_LUA_JOB_TIMEOUT;
    } else if (strcmp(name, "stopped") == 0) {
        *out = CAP_LUA_JOB_STOPPED;
    } else {
        return false;
    }
    return true;
}

static void agent_record_update(const char *task_id, const char *state,
                                const char *message, int progress, bool terminal)
{
    uint8_t status = CAP_LUA_JOB_RUNNING;
    (void)status_from_name(state, &status);

    lock_tasks();
    task_entry_t *entry = find_locked(task_id);
    if (entry == NULL) {
        entry = get_or_create(task_id, task_id, NULL, CLAW_TASK_ORIGIN_AGENT);
    }
    if (entry != NULL) {
        entry->origin = (uint8_t)CLAW_TASK_ORIGIN_AGENT;
        entry->status = status;
        if (progress >= 0) {
            entry->progress = progress > 100 ? 100 : progress;
        }
        if (message != NULL) {
            strlcpy(entry->message, message, sizeof(entry->message));
        }
        if (terminal) {
            entry->ended_ms = now_ms();
        }
    }
    unlock_tasks();
}

void claw_task_agent_begin(const char *task_id, const char *name, const char *prompt)
{
    if (task_id == NULL || task_id[0] == '\0') {
        return;
    }

    lock_tasks();
    task_entry_t *entry = get_or_create(task_id,
                                        name && name[0] ? name : task_id,
                                        prompt, CLAW_TASK_ORIGIN_AGENT);
    if (entry != NULL) {
        entry->origin = (uint8_t)CLAW_TASK_ORIGIN_AGENT;
        entry->status = CAP_LUA_JOB_QUEUED;
        entry->progress = 0;
        strlcpy(entry->message, "queued", sizeof(entry->message));
    }
    unlock_tasks();
}

void claw_task_agent_update(const char *task_id, const char *state,
                            const char *message, int progress)
{
    agent_record_update(task_id, state, message, progress, false);
}

void claw_task_agent_finish(const char *task_id, const char *state,
                            const char *message)
{
    agent_record_update(task_id, state, message, 100, true);
}

bool claw_task_remove(const char *job_id)
{
    bool removed = false;
    lock_tasks();
    task_entry_t *entry = find_locked(job_id);
    if (entry != NULL && !status_is_active(entry->status)) {
        memset(entry, 0, sizeof(*entry));
        removed = true;
    }
    unlock_tasks();
    return removed;
}

size_t claw_task_clear_finished(void)
{
    size_t count = 0;
    lock_tasks();
    for (size_t i = 0; i < CLAW_TASK_SLOTS; i++) {
        if (s_tasks[i].used && !status_is_active(s_tasks[i].status)) {
            memset(&s_tasks[i], 0, sizeof(s_tasks[i]));
            count++;
        }
    }
    unlock_tasks();
    return count;
}

esp_err_t claw_task_registry_snapshot_json(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t order[CLAW_TASK_SLOTS];
    size_t count = 0;

    lock_tasks();
    for (size_t i = 0; i < CLAW_TASK_SLOTS; i++) {
        if (!s_tasks[i].used) {
            continue;
        }
        /* Insertion sort, newest first. */
        size_t pos = count;
        while (pos > 0 && s_tasks[order[pos - 1]].started_ms < s_tasks[i].started_ms) {
            order[pos] = order[pos - 1];
            pos--;
        }
        order[pos] = i;
        count++;
    }

    cJSON *array = cJSON_CreateArray();
    if (!array) {
        unlock_tasks();
        return ESP_ERR_NO_MEM;
    }
    for (size_t n = 0; n < count; n++) {
        const task_entry_t *entry = &s_tasks[order[n]];
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        cJSON_AddStringToObject(item, "id", entry->job_id);
        cJSON_AddStringToObject(item, "name", entry->name);
        cJSON_AddStringToObject(item, "origin",
                                claw_task_origin_name((claw_task_origin_t)entry->origin));
        cJSON_AddStringToObject(item, "state",
                                cap_lua_job_status_name((cap_lua_job_status_t)entry->status));
        cJSON_AddStringToObject(item, "path", entry->path);
        cJSON_AddNumberToObject(item, "progress", entry->progress);
        cJSON_AddStringToObject(item, "message", entry->message);
        cJSON_AddNumberToObject(item, "started_ms", (double)entry->started_ms);
        cJSON_AddNumberToObject(item, "ended_ms", (double)entry->ended_ms);
        cJSON_AddItemToArray(array, item);
    }
    unlock_tasks();

    char *text = cJSON_PrintUnformatted(array);
    cJSON_Delete(array);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(out, text, out_size);
    cJSON_free(text);
    return ESP_OK;
}
