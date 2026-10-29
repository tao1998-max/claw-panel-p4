/*
 * Optional local ESP-Claw Agent (CONFIG_TEMPLATE_CLAW_AGENT).
 *
 * Design notes:
 *  - Nothing is created at boot. The core, memory and skill registry start on
 *    the first request, after the user enables the agent in NVS.
 *  - Requests are serialized by s_lock so the core always owns one run; the
 *    response for that run is awaited with a bounded timeout.
 *  - The core worker task runs on an internal-RAM stack (see claw_core.c), so
 *    capability calls may write FAT/NVS with the flash cache disabled.
 *  - This module never touches LVGL. The page polls status_json/last_reply.
 */
#include "claw_agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "claw_api.h"
#include "claw_panel.h"
#include "claw_settings.h"
#include "claw_storage.h"
#include "claw_tasks.h"

#if CONFIG_TEMPLATE_CLAW_AGENT

#include "cap_agent_mgr.h"
#include "cap_session_mgr.h"
#include "cap_skill_mgr.h"
#include "claw_agent_mgr.h"
#include "claw_cap.h"
#include "claw_core.h"
#include "claw_memory.h"
#include "claw_skill.h"

/*
 * Development-only one-shot provisioning. The header is gitignored and only
 * compiled when TEMPLATE_CLAW_AGENT_PROVISION is enabled; it is not part of
 * production builds. Values are written to NVS only while no key is stored.
 */
#if CONFIG_TEMPLATE_CLAW_AGENT_PROVISION
#if defined(__has_include)
#if __has_include("claw_agent_provision.h")
#include "claw_agent_provision.h"
#define CLAW_AGENT_PROVISION_READY 1
#endif
#endif
#ifndef CLAW_AGENT_PROVISION_READY
#warning "TEMPLATE_CLAW_AGENT_PROVISION is on but claw_agent_provision.h is missing"
#endif
#endif

static const char *TAG = "claw_agent";

#define AGENT_SESSION_ID        "local"
#define AGENT_TASK_PREFIX       "agent-"
#define AGENT_TASK_ID_SIZE      16
#define AGENT_WORKER_STACK      8192
#define AGENT_WORKER_PRIORITY   3
#define AGENT_CORE_STACK        (16 * 1024)
#define AGENT_CORE_TIMEOUT_MS   60000
#define AGENT_CORE_MAX_TOKENS   3072
/* Upstream CONFIG_APP_CLAW_MAX_TOOL_ITERATIONS defaults to 64; a launcher App
 * needs resolve + spec + authoring + read-back + publish in one turn. */
#define AGENT_CORE_MAX_ITER     64
#define AGENT_SYNC_TIMEOUT_MAX  120000
#define AGENT_OUTPUT_MAX        2048
#define AGENT_ERROR_MAX         160
#define AGENT_STATE_MAX         16

#define AGENT_GROUP_ID "cap_claw"

static const char *AGENT_SYSTEM_PROMPT =
    "You are \"小爪\" (ESP-Claw), the local automation agent inside an ESP32-P4 "
    "industrial panel. XiaoZhi, a cloud voice assistant, forwards short user "
    "requests to you. Reply in the user's language (usually Chinese), briefly "
    "and plainly; your answer may be spoken by TTS.\n"
    "\n"
    "You act only through tools:\n"
    "- list_skill / activate_skill: reusable workflows. Activate a skill when "
    "its catalog entry matches the request.\n"
    "- read_file / write_file / list_dir: workspace files. Paths are absolute; "
    "scripts live under /fatfs/claw/data/scripts/<name>.lua (writable). "
    "/fatfs/claw/system is read-only. Never use \"..\".\n"
    "- lua_run_script: run a script that finishes in a few seconds. "
    "lua_run_script_async: anything longer; it returns a job id and you must "
    "report that id instead of blocking.\n"
    "- panel_set_brightness / panel_set_volume / panel_show_message: direct "
    "panel actions. Lua can also call claw.panel.* inside a script.\n"
    "- scheduled_task skill: for requests like 10秒后/每隔/每天做某事, activate "
    "this official skill and run its bundled script; it creates a router rule "
    "and a scheduler entry (run_script mode).\n"
    "- memory_ops / profile_memory_ops skills: use only when the user explicitly "
    "asks to remember, update, forget or inspect long-term memory. Ordinary "
    "preferences are extracted automatically after the reply.\n"
    "\n"
    "Rules:\n"
    "- Never expose secrets, never write outside the workspace, never touch "
    "Wi-Fi, NVS, audio or firmware.\n"
    "- Do not call yourself recursively or manage the agent itself.\n"
    "- Final answers: current status, useful result, clear next step.\n";

static const char *AGENT_LUA_GUIDE =
    "Lua runtime: official ESP-Claw cap_lua (standard Lua libraries are available).\n"
    "Prefer the `claw` table for device work:\n"
    "  claw.log(...), claw.sleep(ms), claw.time(), claw.uptime_ms(),\n"
    "  claw.progress(pct, msg), claw.abort_requested(), claw.get_args(),\n"
    "  claw.file.read/write/list/exists/remove(rel_path) (data/ is writable, "
    "write <=16 KB, read <=64 KB),\n"
    "  claw.json.encode(value), claw.json.decode(text),\n"
    "  claw.panel.set_volume(pct), claw.panel.set_brightness(pct),\n"
    "  claw.panel.status(), claw.panel.show_message(text), claw.panel.demo(on).\n"
    "Official skills also use require(\"capability\"), require(\"json\") and "
    "require(\"arg_schema\").\n"
    "This build has LVGL tiny_ttf disabled: lvgl.font_load() may raise. Wrap "
    "it in pcall and fall back to the built-in font.\n"
    "Script template:\n"
    "  claw.progress(10, \"start\")\n"
    "  claw.panel.set_brightness(40)\n"
    "  claw.sleep(1000)\n"
    "  claw.panel.set_brightness(100)\n"
    "  print(\"done\")\n"
    "Save it as /fatfs/claw/data/scripts/<name>.lua, then call lua_run_script "
    "with that absolute path.\n";

/* ---------- state ---------- */

static SemaphoreHandle_t s_lock;        /* one agent request at a time */
static SemaphoreHandle_t s_state_lock;  /* protects the fields below */
static claw_core_handle_t s_core;
static bool s_resources_ready;
static bool s_agent_mgr_inited;
static bool s_agent_mgr_ready;
static bool s_dirty;
static bool s_worker_running;
static bool s_stop_requested;
static uint32_t s_task_seq;
static uint32_t s_inflight_request;
static uint32_t s_reply_seq;
static char *s_last_reply;              /* PSRAM, AGENT_OUTPUT_MAX */
static char *s_last_task_output;        /* PSRAM, AGENT_OUTPUT_MAX */
static char s_last_error[AGENT_ERROR_MAX];
static char s_last_task_id[AGENT_TASK_ID_SIZE];
static char s_last_task_state[AGENT_STATE_MAX];

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void state_lock(void)
{
    if (s_state_lock) {
        xSemaphoreTake(s_state_lock, portMAX_DELAY);
    }
}

static void state_unlock(void)
{
    if (s_state_lock) {
        xSemaphoreGive(s_state_lock);
    }
}

static void state_set_error(const char *message)
{
    state_lock();
    strlcpy(s_last_error, message ? message : "", sizeof(s_last_error));
    state_unlock();
}

static void state_set_reply(const char *text)
{
    state_lock();
    if (s_last_reply) {
        strlcpy(s_last_reply, text ? text : "", AGENT_OUTPUT_MAX);
    }
    s_reply_seq++;
    s_last_error[0] = '\0';
    state_unlock();
}

static void state_set_inflight(uint32_t request_id)
{
    state_lock();
    s_inflight_request = request_id;
    state_unlock();
}

static uint32_t state_get_inflight(void)
{
    state_lock();
    uint32_t id = s_inflight_request;
    state_unlock();
    return id;
}

static void state_set_last_task(const char *task_id, const char *state, const char *output)
{
    state_lock();
    strlcpy(s_last_task_id, task_id ? task_id : "", sizeof(s_last_task_id));
    strlcpy(s_last_task_state, state ? state : "", sizeof(s_last_task_state));
    if (s_last_task_output) {
        strlcpy(s_last_task_output, output ? output : "", AGENT_OUTPUT_MAX);
    }
    s_stop_requested = false;
    state_unlock();
}

/* ---------- capability group: panel + scheduling helpers ---------- */

static int json_int(cJSON *root, const char *key, int default_value)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    return cJSON_IsNumber(item) ? item->valueint : default_value;
}

static esp_err_t cap_panel_set_volume(const char *input_json,
                                      const claw_cap_call_context_t *ctx,
                                      char *output, size_t output_size)
{
    (void)ctx;
    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    int percent = json_int(root, "percent", -1);
    cJSON_Delete(root);
    if (percent < 0) {
        snprintf(output, output_size, "Error: percent is required");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = claw_panel_set_volume(percent);
    snprintf(output, output_size, err == ESP_OK ? "OK: volume set" : "Error: %s",
             esp_err_to_name(err));
    return err;
}

static esp_err_t cap_panel_set_brightness(const char *input_json,
                                          const claw_cap_call_context_t *ctx,
                                          char *output, size_t output_size)
{
    (void)ctx;
    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    int percent = json_int(root, "percent", -1);
    cJSON_Delete(root);
    if (percent < 0) {
        snprintf(output, output_size, "Error: percent is required");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = claw_panel_set_brightness(percent);
    snprintf(output, output_size, err == ESP_OK ? "OK: brightness set" : "Error: %s",
             esp_err_to_name(err));
    return err;
}

static esp_err_t cap_panel_status(const char *input_json,
                                  const claw_cap_call_context_t *ctx,
                                  char *output, size_t output_size)
{
    (void)input_json;
    (void)ctx;
    return claw_panel_status(output, output_size);
}

static esp_err_t cap_panel_show_message(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output, size_t output_size)
{
    (void)ctx;
    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (text && text[0]) {
        err = claw_panel_show_message(text);
    }
    cJSON_Delete(root);
    if (err == ESP_OK) {
        snprintf(output, output_size, "OK: message shown");
    } else {
        snprintf(output, output_size, "Error: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t cap_panel_demo(const char *input_json,
                                const claw_cap_call_context_t *ctx,
                                char *output, size_t output_size)
{
    (void)ctx;
    cJSON *root = cJSON_Parse(input_json ? input_json : "{}");
    cJSON *on_item = cJSON_GetObjectItem(root, "on");
    bool on = cJSON_IsTrue(on_item);
    cJSON_Delete(root);
    esp_err_t err = claw_panel_demo(on);
    snprintf(output, output_size, err == ESP_OK ? "OK: demo updated" : "Error: %s",
             esp_err_to_name(err));
    return err;
}

static const claw_cap_descriptor_t s_agent_caps[] = {
    {
        .id = "panel_set_volume",
        .name = "panel_set_volume",
        .family = "panel",
        .description = "Set the speaker volume of this panel (0-100).",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"percent\":{\"type\":\"integer\","
        "\"minimum\":0,\"maximum\":100}},\"required\":[\"percent\"]}",
        .execute = cap_panel_set_volume,
    },
    {
        .id = "panel_set_brightness",
        .name = "panel_set_brightness",
        .family = "panel",
        .description = "Set the display brightness of this panel (0-100).",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"percent\":{\"type\":\"integer\","
        "\"minimum\":0,\"maximum\":100}},\"required\":[\"percent\"]}",
        .execute = cap_panel_set_brightness,
    },
    {
        .id = "panel_status",
        .name = "panel_status",
        .family = "panel",
        .description = "Read panel volume, brightness and network status.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_panel_status,
    },
    {
        .id = "panel_show_message",
        .name = "panel_show_message",
        .family = "panel",
        .description = "Show a short message bubble on the panel screen.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},"
        "\"required\":[\"text\"]}",
        .execute = cap_panel_show_message,
    },
    {
        .id = "panel_demo",
        .name = "panel_demo",
        .family = "panel",
        .description = "Turn the home page demo switch on or off.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"on\":{\"type\":\"boolean\"}},"
        "\"required\":[\"on\"]}",
        .execute = cap_panel_demo,
    },
};

static const claw_cap_group_t s_agent_group = {
    .group_id = AGENT_GROUP_ID,
    .plugin_name = "claw_port",
    .version = "1.0.0",
    .descriptors = s_agent_caps,
    .descriptor_count = sizeof(s_agent_caps) / sizeof(s_agent_caps[0]),
};

static bool agent_tool_authorizer(const char *group_id,
                                  const claw_cap_descriptor_t *descriptor,
                                  const claw_cap_call_context_t *ctx,
                                  void *user_ctx)
{
    (void)ctx;
    (void)user_ctx;
    if (descriptor == NULL) {
        return false;
    }
    /* The user-facing AI-Lua switch also gates the local agent's Lua tools. */
    if (group_id != NULL && strcmp(group_id, "cap_lua") == 0 &&
        !claw_settings_lua_ai_enabled()) {
        return false;
    }
    /* Skill management stays available only through management skills. */
    if (strcmp(descriptor->id, "publish_skill") == 0 ||
        strcmp(descriptor->id, "remove_skill") == 0) {
        return false;
    }
    return true;
}

/* ---------- context providers ---------- */

static esp_err_t lua_guide_collect(const claw_core_request_t *request,
                                   claw_core_context_t *out_context,
                                   void *user_ctx)
{
    (void)request;
    (void)user_ctx;
    char *copy = strdup(AGENT_LUA_GUIDE);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    out_context->kind = CLAW_CORE_CONTEXT_KIND_SYSTEM_PROMPT;
    out_context->content = copy;
    return ESP_OK;
}

static const claw_core_context_provider_t s_lua_guide_provider = {
    .name = "Lua Guide",
    .collect = lua_guide_collect,
};

typedef struct {
    char *text;
    size_t length;
} skills_builder_t;

static esp_err_t skills_append(const claw_skill_catalog_entry_t *entry, void *user_ctx)
{
    skills_builder_t *builder = user_ctx;
    if (entry == NULL || builder == NULL || entry->id == NULL) {
        return ESP_OK;
    }
    const char *summary = entry->summary ? entry->summary : "";
    int added = snprintf(NULL, 0, "- %s: %s\n", entry->id, summary);
    if (added < 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *grown = realloc(builder->text, builder->length + (size_t)added + 1);
    if (grown == NULL) {
        return ESP_ERR_NO_MEM;
    }
    builder->text = grown;
    snprintf(builder->text + builder->length, (size_t)added + 1,
             "- %s: %s\n", entry->id, summary);
    builder->length += (size_t)added;
    return ESP_OK;
}

static esp_err_t skills_collect(const claw_core_request_t *request,
                                claw_core_context_t *out_context,
                                void *user_ctx)
{
    (void)request;
    (void)user_ctx;
    static const char heading[] = "Available skills (activate_skill loads one):\n";
    skills_builder_t builder = {
        .text = strdup(heading),
        .length = sizeof(heading) - 1,
    };
    if (builder.text == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = claw_skill_foreach_catalog_entry(skills_append, &builder);
    if (err != ESP_OK || builder.length <= sizeof(heading) - 1) {
        free(builder.text);
        return err != ESP_OK ? err : ESP_ERR_NOT_FOUND;
    }
    out_context->kind = CLAW_CORE_CONTEXT_KIND_SYSTEM_PROMPT;
    out_context->content = builder.text;
    return ESP_OK;
}

static const claw_core_context_provider_t s_skills_provider = {
    .name = "Skills List",
    .collect = skills_collect,
};

/* ---------- lifecycle ---------- */

static esp_err_t agent_delete_session_history(const char *session_id,
                                              bool *out_deleted_any,
                                              void *user_ctx)
{
    (void)user_ctx;
    if (!session_id || !out_deleted_any) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_deleted_any = false;
    bool deleted = false;
    esp_err_t err = ESP_OK;
#if CONFIG_TEMPLATE_CLAW_AGENT_MEMORY
    err = claw_memory_delete_session_history(session_id, &deleted);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    *out_deleted_any |= deleted;
#endif
    deleted = false;
    err = claw_skill_delete_session_state(session_id, &deleted);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    *out_deleted_any |= deleted;
    return ESP_OK;
}

static esp_err_t agent_init_resources(const claw_agent_settings_t *cfg)
{
    if (s_resources_ready) {
        return ESP_OK;
    }

    esp_err_t err = claw_skill_init(&(claw_skill_config_t) {
        .session_state_root_dir = CLAW_STORAGE_DATA_DIR "/agent/sessions",
        .max_file_bytes = 20 * 1024,
    });
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "skill init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = claw_skill_add_directory(CLAW_STORAGE_DATA_DIR "/skills");
    if (err != ESP_OK) {
        return err;
    }
    err = claw_skill_add_directory(CLAW_STORAGE_SYSTEM_DIR "/skills");
    if (err != ESP_OK) {
        return err;
    }
    err = claw_skill_reload_registry();
    if (err != ESP_OK) {
        return err;
    }

#if CONFIG_TEMPLATE_CLAW_AGENT_MEMORY
    err = claw_memory_init(&(claw_memory_config_t) {
        .session_root_dir = CLAW_STORAGE_DATA_DIR "/agent/sessions",
        .memory_root_dir = CLAW_STORAGE_DATA_DIR "/agent/memory",
        .max_message_chars = 4096,
        .max_tool_iterations = AGENT_CORE_MAX_ITER,
        .llm = {
            .api_key = cfg->api_key,
            .backend_type = cfg->backend,
            .endpoint_provider = cfg->endpoint_provider[0]
                                     ? cfg->endpoint_provider : NULL,
            .model = cfg->model,
            .base_url = cfg->base_url,
            .auth_type = cfg->auth_type[0] ? cfg->auth_type : NULL,
            .max_tokens_field = cfg->max_tokens_field[0]
                                    ? cfg->max_tokens_field : NULL,
            .reasoning_effort = cfg->reasoning_effort[0]
                                    ? cfg->reasoning_effort : NULL,
            .timeout_ms = cfg->timeout_ms ? cfg->timeout_ms : AGENT_CORE_TIMEOUT_MS,
            .max_tokens = cfg->max_tokens ? cfg->max_tokens : AGENT_CORE_MAX_TOKENS,
            .image_max_bytes = cfg->image_max_bytes,
            .supports_tools = cfg->supports_tools,
            .supports_vision = cfg->supports_vision,
            .image_remote_url_only = cfg->image_remote_url_only,
        },
        .enable_async_extract_stage_note = true,
    });
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "memory init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = claw_memory_register_group();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "memory cap group failed: %s", esp_err_to_name(err));
        return err;
    }
#else
    (void)cfg;
    ESP_LOGI(TAG, "agent memory disabled; turns are stateless");
#endif

    err = cap_skill_mgr_register_group();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "skill cap group failed: %s", esp_err_to_name(err));
        return err;
    }

#if CONFIG_TEMPLATE_CLAW_AGENT_MEMORY
    static const char *const visible[] = {
        AGENT_GROUP_ID, "cap_files", "cap_lua", "cap_scheduler",
        "cap_router_mgr", "cap_skill", "cap_system", "cap_llm_inspect",
        "cap_http_request", "cap_web_search", "cap_agent_mgr", "claw_memory",
        "cap_app_manage",
    };
#else
    static const char *const visible[] = {
        AGENT_GROUP_ID, "cap_files", "cap_lua", "cap_scheduler",
        "cap_router_mgr", "cap_skill", "cap_system", "cap_llm_inspect",
        "cap_http_request", "cap_web_search", "cap_agent_mgr",
        "cap_app_manage",
    };
#endif
    err = claw_cap_set_llm_visible_groups(visible, sizeof(visible) / sizeof(visible[0]));
    if (err != ESP_OK) {
        return err;
    }
    err = claw_cap_set_llm_authorizer(agent_tool_authorizer, NULL);
    if (err != ESP_OK) {
        return err;
    }

    s_resources_ready = true;
    return ESP_OK;
}

static void agent_fill_core_config(const claw_agent_settings_t *cfg,
                                   claw_core_config_t *out_config)
{
    bool anthropic = strcmp(cfg->backend, "anthropic_compatible") == 0;
    const char *auth_type = cfg->auth_type[0]
                                ? cfg->auth_type
                                : (cfg->api_key[0] == '\0'
                                       ? "none" : (anthropic ? "api-key" : "bearer"));

    *out_config = (claw_core_config_t) {
        .instance_id = 0,
        .api_key = cfg->api_key,
        .backend_type = cfg->backend,
        .endpoint_provider = cfg->endpoint_provider[0]
                                 ? cfg->endpoint_provider : NULL,
        .model = cfg->model,
        .base_url = cfg->base_url,
        .auth_type = auth_type,
        .max_tokens_field = cfg->max_tokens_field[0]
                                ? cfg->max_tokens_field : NULL,
        .reasoning_effort = cfg->reasoning_effort[0]
                                ? cfg->reasoning_effort : NULL,
        .timeout_ms = cfg->timeout_ms ? cfg->timeout_ms : AGENT_CORE_TIMEOUT_MS,
        .max_tokens = cfg->max_tokens ? cfg->max_tokens : AGENT_CORE_MAX_TOKENS,
        .image_max_bytes = cfg->image_max_bytes,
        .supports_tools = cfg->supports_tools,
        .supports_vision = cfg->supports_vision,
        .image_remote_url_only = cfg->image_remote_url_only,
        .system_prompt = AGENT_SYSTEM_PROMPT,
#if CONFIG_TEMPLATE_CLAW_AGENT_MEMORY
        .persist_context = claw_memory_persist_context_callback,
        .request_gate = claw_memory_request_gate_callback,
        .on_request_start = claw_memory_request_start_callback,
        .collect_stage_note = claw_memory_stage_note_callback,
#endif
        .call_cap = claw_cap_call_from_core,
        .task_stack_size = AGENT_CORE_STACK,
        .task_priority = AGENT_WORKER_PRIORITY,
        .task_core = tskNO_AFFINITY,
        .max_tool_iterations = AGENT_CORE_MAX_ITER,
        .request_queue_len = 2,
        .response_queue_len = 2,
        .max_context_providers = 8,
    };
}

static esp_err_t agent_init_manager(const claw_agent_settings_t *cfg)
{
    claw_core_config_t core_cfg;
    agent_fill_core_config(cfg, &core_cfg);

    if (!s_agent_mgr_inited) {
        /* agent_mgr adds the root/sub-agent cap-tools provider itself. Memory,
         * skills and the Lua guide are the product's base providers. */
        const claw_core_context_provider_t providers[] = {
#if CONFIG_TEMPLATE_CLAW_AGENT_MEMORY
            claw_memory_profile_provider,
            claw_memory_long_term_provider,
            claw_memory_session_history_provider,
#endif
            s_skills_provider,
            s_lua_guide_provider,
        };
        const claw_agent_mgr_config_t mgr_cfg = {
            .core_config = &core_cfg,
            .base_context_providers = providers,
            .base_context_provider_count = sizeof(providers) / sizeof(providers[0]),
            .root_agent_system_prompt = NULL,
            .subagent_system_prompt = NULL,
            .subagent_type_prompts = NULL,
            .subagent_type_prompt_count = 0,
        };
        esp_err_t err = claw_agent_mgr_init(&mgr_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "agent manager init failed: %s", esp_err_to_name(err));
            return err;
        }
        s_agent_mgr_inited = true;
    }

    if (s_core == NULL) {
        const char *root_id = NULL;
        esp_err_t err = claw_agent_mgr_create_root_agent(&root_id);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "root agent create failed: %s", esp_err_to_name(err));
            return err;
        }
        s_core = claw_agent_mgr_get_root_core();
        if (s_core == NULL) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    s_agent_mgr_ready = true;
    ESP_LOGI(TAG, "local agent ready via claw_agent_mgr backend=%s model=%s",
             cfg->backend, cfg->model);
    return ESP_OK;
}

static esp_err_t agent_update_manager_config(const claw_agent_settings_t *cfg)
{
    if (!s_agent_mgr_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    claw_core_config_t core_cfg;
    agent_fill_core_config(cfg, &core_cfg);
    return claw_agent_mgr_update_core_config(&core_cfg);
}

static esp_err_t agent_ensure_ready(void)
{
    if (!claw_settings_agent_enabled()) {
        return ESP_ERR_INVALID_STATE;
    }
    claw_agent_settings_t cfg;
    if (claw_settings_get_agent(&cfg) != ESP_OK || !claw_settings_agent_configured()) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_agent_mgr_ready && s_dirty) {
        esp_err_t err = agent_update_manager_config(&cfg);
        if (err != ESP_OK) {
            return err;
        }
        s_dirty = false;
    }
    if (s_agent_mgr_ready) {
        return s_core != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = agent_init_resources(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    return agent_init_manager(&cfg);
}

#if defined(CLAW_AGENT_PROVISION_READY)
#ifndef CLAW_AGENT_PROVISION_BACKEND
#define CLAW_AGENT_PROVISION_BACKEND "openai_compatible"
#endif
#ifndef CLAW_AGENT_PROVISION_BASE_URL
#define CLAW_AGENT_PROVISION_BASE_URL ""
#endif
#ifndef CLAW_AGENT_PROVISION_MODEL
#define CLAW_AGENT_PROVISION_MODEL ""
#endif
#ifndef CLAW_AGENT_PROVISION_ENABLE
#define CLAW_AGENT_PROVISION_ENABLE 1
#endif

static void agent_provision_once(void)
{
    if (claw_settings_provision_done()) {
        return;
    }
    if (strlen(CLAW_AGENT_PROVISION_KEY) == 0) {
        return;
    }

    claw_agent_settings_t cfg;
    if (claw_settings_get_agent(&cfg) != ESP_OK) {
        return;
    }

    /* Fill only what is missing: never overwrite a key/URL/model the user
     * edited. This also repairs a device that has a key but no URL/model. */
    esp_err_t err = ESP_OK;
    if (cfg.api_key[0] == '\0') {
        err = claw_settings_set_agent_api_key(CLAW_AGENT_PROVISION_KEY);
    }
    if (err == ESP_OK && cfg.base_url[0] == '\0' &&
        CLAW_AGENT_PROVISION_BASE_URL[0] != '\0') {
        err = claw_settings_set_agent_base_url(CLAW_AGENT_PROVISION_BASE_URL);
    }
    if (err == ESP_OK && cfg.model[0] == '\0' &&
        CLAW_AGENT_PROVISION_MODEL[0] != '\0') {
        err = claw_settings_set_agent_model(CLAW_AGENT_PROVISION_MODEL);
    }
    if (err == ESP_OK && CLAW_AGENT_PROVISION_ENABLE) {
        err = claw_settings_set_agent(true);
    }
    if (err == ESP_OK) {
        (void)claw_settings_set_provision_done(true);
        ESP_LOGI(TAG, "agent provisioned from build-time defaults (model=%s, switch=%s)",
                 cfg.model[0] ? cfg.model : CLAW_AGENT_PROVISION_MODEL,
                 CLAW_AGENT_PROVISION_ENABLE ? "on" : "unchanged");
    } else {
        ESP_LOGE(TAG, "agent provisioning failed: %s", esp_err_to_name(err));
    }
}
#endif /* CLAW_AGENT_PROVISION_READY */

esp_err_t claw_agent_init(void)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    s_state_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL || s_state_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_last_reply = heap_caps_calloc(1, AGENT_OUTPUT_MAX,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_last_task_output = heap_caps_calloc(1, AGENT_OUTPUT_MAX,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_last_reply == NULL || s_last_task_output == NULL) {
        ESP_LOGW(TAG, "no PSRAM for reply buffers; replies will be shortened");
    }

    esp_err_t err = cap_session_mgr_set_session_root_dir(
        CLAW_STORAGE_DATA_DIR "/agent/sessions");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "session manager root failed: %s", esp_err_to_name(err));
        return err;
    }
    err = cap_session_mgr_set_delete_session_handler(agent_delete_session_history, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "session delete handler failed: %s", esp_err_to_name(err));
        return err;
    }

    if (!claw_cap_group_exists(AGENT_GROUP_ID)) {
        esp_err_t err = claw_cap_register_group(&s_agent_group);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", AGENT_GROUP_ID,
                     esp_err_to_name(err));
            return err;
        }
    }

#if defined(CLAW_AGENT_PROVISION_READY)
    agent_provision_once();
#endif

    ESP_LOGI(TAG, "local agent module ready (switch %s)",
             claw_settings_agent_enabled() ? "on" : "off");
    return ESP_OK;
}

esp_err_t claw_agent_shutdown(uint32_t timeout_ms)
{
    if (s_lock == NULL) {
        return ESP_OK;
    }
    /* Ask the in-flight request to abort first so the lock is released soon. */
    uint32_t inflight = state_get_inflight();
    if (inflight != 0 && s_core != NULL) {
        (void)claw_agent_mgr_cancel_root_request(inflight);
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(timeout_ms ? timeout_ms : 3000)) != pdTRUE) {
        ESP_LOGW(TAG, "shutdown: request still running");
        return ESP_ERR_TIMEOUT;
    }
    /* agent_mgr has no deinit API; a factory reset reboots immediately after
     * this call. Keep the root core parked and refresh it on re-enable. */
    s_dirty = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void claw_agent_settings_changed(void)
{
    if (!claw_settings_agent_enabled()) {
        uint32_t inflight = state_get_inflight();
        if (inflight != 0) {
            (void)claw_agent_mgr_cancel_root_request(inflight);
        }
        return;
    }
    if (s_lock == NULL || !s_agent_mgr_ready) {
        return;
    }
    claw_agent_settings_t cfg;
    if (claw_settings_get_agent(&cfg) != ESP_OK) {
        return;
    }
    if (xSemaphoreTake(s_lock, 0) == pdTRUE) {
        esp_err_t err = agent_update_manager_config(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "agent config update failed: %s", esp_err_to_name(err));
        }
        s_dirty = false;
        xSemaphoreGive(s_lock);
    } else {
        /* Apply after the current request finishes. */
        s_dirty = true;
    }
}

esp_err_t claw_agent_prepare(void)
{
    if (!claw_settings_agent_enabled()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!claw_settings_agent_configured()) {
        return ESP_ERR_INVALID_ARG;
    }
    /* The manager and root core are created once. Reading them without the
     * lock is safe here: they are only assigned while holding s_lock, and
     * claw_agent_is_ready() already does the same. */
    if (s_agent_mgr_ready) {
        return s_core != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = agent_ensure_ready();
    xSemaphoreGive(s_lock);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "agent manager prepared");
    }
    return err;
}

bool claw_agent_is_ready(void)
{
    return s_agent_mgr_ready && s_core != NULL &&
           claw_settings_agent_enabled();
}

bool claw_agent_is_busy(void)
{
    state_lock();
    bool busy = s_worker_running || s_inflight_request != 0;
    state_unlock();
    return busy;
}

/* ---------- requests ---------- */

esp_err_t claw_agent_ask(const char *prompt, const char *origin,
                         uint32_t timeout_ms, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (prompt == NULL || prompt[0] == '\0') {
        snprintf(out, out_size, "Error: empty prompt");
        return ESP_ERR_INVALID_ARG;
    }
    if (timeout_ms == 0) {
        timeout_ms = AGENT_CORE_TIMEOUT_MS;
    }
    if (timeout_ms > AGENT_SYNC_TIMEOUT_MAX) {
        timeout_ms = AGENT_SYNC_TIMEOUT_MAX;
    }
    if (!claw_settings_agent_enabled()) {
        snprintf(out, out_size,
                 "Error: 本地 Agent 未开启。请在 Claw 页面 → 设置 中配置并开启。");
        return ESP_ERR_INVALID_STATE;
    }
    if (!claw_settings_agent_configured()) {
        snprintf(out, out_size,
                 "Error: Agent 配置不完整（需要接口地址和模型名称）。");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        snprintf(out, out_size, "Error: 小爪正在处理上一个请求，请稍后再试。");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = agent_ensure_ready();
    if (err != ESP_OK) {
        state_set_error("agent init failed");
        snprintf(out, out_size, "Error: 本地 Agent 初始化失败 (%s)",
                 esp_err_to_name(err));
        goto done;
    }

    const char *session_state = claw_core_session_state(s_core, AGENT_SESSION_ID);
    if (session_state != NULL && strcmp(session_state, "idle") != 0) {
        snprintf(out, out_size, "Error: 上一次请求仍在结束中，请稍后再试。");
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }

    const claw_agent_mgr_root_input_t input = {
        .session_id = AGENT_SESSION_ID,
        .session_policy = CLAW_SESSION_POLICY_CHAT,
        .user_text = prompt,
        .source_channel = origin ? origin : "local",
        .source_cap = "claw_agent",
    };
    claw_core_message_receipt_t receipt = {0};

    err = claw_agent_mgr_post_root_message(&input, 2000, &receipt);
    if (err != ESP_OK) {
        state_set_inflight(0);
        state_set_error("queue failed");
        snprintf(out, out_size, "Error: 无法提交给本地 Agent (%s)",
                 esp_err_to_name(err));
        goto done;
    }

    uint32_t wait_id = receipt.run_id;
    state_set_inflight(wait_id);

    claw_core_response_t response = {0};
    err = claw_agent_mgr_receive_root_for(wait_id, &response, timeout_ms);
    if (err == ESP_OK) {
        if (response.status != CLAW_CORE_RESPONSE_STATUS_OK) {
            const char *message = response.error_message ? response.error_message
                                                         : "agent request failed";
            state_set_error(message);
            snprintf(out, out_size, "Error: %s", message);
            err = ESP_FAIL;
        } else {
            const char *text = (response.text && response.text[0])
                               ? response.text : "(no answer)";
            state_set_reply(text);
            strlcpy(out, text, out_size);
            err = ESP_OK;
        }
        claw_core_response_free(&response);
    } else if (err == ESP_ERR_TIMEOUT) {
        (void)claw_agent_mgr_cancel_root_request(wait_id);
        state_set_error("timeout");
        snprintf(out, out_size,
                 "Error: 本地 Agent 超时（%u ms），本次请求已取消。",
                 (unsigned)timeout_ms);
    } else {
        state_set_error("receive failed");
        snprintf(out, out_size, "Error: 读取 Agent 回复失败 (%s)",
                 esp_err_to_name(err));
    }
    state_set_inflight(0);

done:
    xSemaphoreGive(s_lock);
    return err;
}

/* ---------- background task ---------- */

typedef struct {
    char id[AGENT_TASK_ID_SIZE];
    char *name;
    char *prompt;
    uint32_t timeout_ms;
} agent_job_t;

static void agent_worker(void *arg)
{
    agent_job_t *job = arg;

    claw_task_agent_update(job->id, "running", "thinking", 10);

    char reply[1024] = {0};
    esp_err_t err = claw_agent_ask(job->prompt, "agent", job->timeout_ms,
                                   reply, sizeof(reply));

    bool stopped;
    state_lock();
    stopped = s_stop_requested;
    s_stop_requested = false;
    state_unlock();

    const char *state = (err == ESP_OK) ? "done"
                        : stopped ? "stopped"
                        : (err == ESP_ERR_TIMEOUT ? "timeout" : "failed");
    const char *output = reply[0] ? reply : "request failed";

    state_set_last_task(job->id, state, output);
    claw_task_agent_finish(job->id, state, output);

    state_lock();
    s_worker_running = false;
    state_unlock();

    free(job->name);
    free(job->prompt);
    free(job);
    vTaskDelete(NULL);
}

esp_err_t claw_agent_start_task(const char *name, const char *prompt,
                                uint32_t timeout_ms, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (prompt == NULL || prompt[0] == '\0') {
        snprintf(out, out_size, "Error: empty prompt");
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_settings_agent_enabled()) {
        snprintf(out, out_size,
                 "Error: 本地 Agent 未开启。请在 Claw 页面 → 设置 中配置并开启。");
        return ESP_ERR_INVALID_STATE;
    }
    if (!claw_settings_agent_configured()) {
        snprintf(out, out_size,
                 "Error: Agent 配置不完整（需要接口地址和模型名称）。");
        return ESP_ERR_INVALID_ARG;
    }
    if (timeout_ms == 0) {
        timeout_ms = AGENT_CORE_TIMEOUT_MS;
    }
    if (timeout_ms > AGENT_SYNC_TIMEOUT_MAX) {
        timeout_ms = AGENT_SYNC_TIMEOUT_MAX;
    }

    uint32_t seq;
    char task_id[AGENT_TASK_ID_SIZE];
    state_lock();
    if (s_worker_running) {
        state_unlock();
        snprintf(out, out_size, "Error: 已有一个 Agent 任务在运行。");
        return ESP_ERR_INVALID_STATE;
    }
    s_worker_running = true;
    s_stop_requested = false;
    seq = ++s_task_seq;
    snprintf(task_id, sizeof(task_id), AGENT_TASK_PREFIX "%u", (unsigned)seq);
    strlcpy(s_last_task_id, task_id, sizeof(s_last_task_id));
    strlcpy(s_last_task_state, "running", sizeof(s_last_task_state));
    if (s_last_task_output != NULL) {
        s_last_task_output[0] = '\0';
    }
    state_unlock();

    claw_task_agent_begin(task_id, name, prompt);

    agent_job_t *job = calloc(1, sizeof(*job));
    if (job != NULL) {
        strlcpy(job->id, task_id, sizeof(job->id));
        job->name = name && name[0] ? strdup(name) : NULL;
        job->prompt = strdup(prompt);
        job->timeout_ms = timeout_ms;
    }

    BaseType_t ok = pdFAIL;
    if (job != NULL && job->prompt != NULL) {
        ok = xTaskCreate(agent_worker, "claw_agent", AGENT_WORKER_STACK, job,
                         AGENT_WORKER_PRIORITY, NULL);
    }
    if (ok != pdPASS) {
        claw_task_agent_finish(task_id, "failed", "task create failed");
        state_lock();
        s_worker_running = false;
        strlcpy(s_last_task_state, "failed", sizeof(s_last_task_state));
        if (s_last_task_output != NULL) {
            strlcpy(s_last_task_output, "task create failed", AGENT_OUTPUT_MAX);
        }
        state_unlock();
        if (job != NULL) {
            free(job->name);
            free(job->prompt);
            free(job);
        }
        snprintf(out, out_size, "Error: 无法创建 Agent 任务");
        return ESP_ERR_NO_MEM;
    }

    snprintf(out, out_size, "{\"task_id\":\"%s\",\"status\":\"queued\"}", task_id);
    ESP_LOGI(TAG, "agent task %s started", task_id);
    return ESP_OK;
}

esp_err_t claw_agent_stop_task(const char *task_id, uint32_t wait_ms,
                               char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (!claw_agent_is_agent_task_id(task_id)) {
        snprintf(out, out_size, "Error: not an agent task id");
        return ESP_ERR_NOT_FOUND;
    }
    if (wait_ms == 0) {
        wait_ms = 1500;
    }
    if (wait_ms > 5000) {
        wait_ms = 5000;
    }

    state_lock();
    bool running = s_worker_running;
    bool current = strcmp(task_id, s_last_task_id) == 0;
    s_stop_requested = running;
    uint32_t inflight = s_inflight_request;
    char last_state[AGENT_STATE_MAX];
    strlcpy(last_state, s_last_task_state, sizeof(last_state));
    state_unlock();

    if (!running) {
        if (current) {
            snprintf(out, out_size, "{\"task_id\":\"%s\",\"state\":\"%s\"}",
                     task_id, last_state[0] ? last_state : "done");
            return ESP_OK;
        }
        snprintf(out, out_size, "Error: agent task not running");
        return ESP_ERR_NOT_FOUND;
    }

    if (inflight != 0) {
        (void)claw_agent_mgr_cancel_root_request(inflight);
    }

    int64_t deadline = now_ms() + wait_ms;
    while (now_ms() < deadline) {
        state_lock();
        running = s_worker_running;
        state_unlock();
        if (!running) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (running) {
        snprintf(out, out_size, "{\"task_id\":\"%s\",\"state\":\"stopping\"}", task_id);
        return ESP_ERR_TIMEOUT;
    }
    snprintf(out, out_size, "{\"task_id\":\"%s\",\"state\":\"stopped\"}", task_id);
    return ESP_OK;
}

esp_err_t claw_agent_get_task_output(const char *task_id, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0 || task_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    state_lock();
    bool match = strcmp(task_id, s_last_task_id) == 0;
    bool running = s_worker_running;
    const char *state = s_last_task_state;
    const char *text = s_last_task_output ? s_last_task_output : "";
    if (match) {
        if (running) {
            snprintf(out, out_size, "Agent task %s is running.", task_id);
        } else if (text[0]) {
            snprintf(out, out_size, "Agent task %s (%s): %s", task_id,
                     state[0] ? state : "done", text);
        } else {
            snprintf(out, out_size, "Agent task %s: no output", task_id);
        }
    }
    state_unlock();

    if (!match) {
        snprintf(out, out_size, "Error: agent task not found");
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

bool claw_agent_is_agent_task_id(const char *task_id)
{
    return task_id != NULL &&
           strncmp(task_id, AGENT_TASK_PREFIX, strlen(AGENT_TASK_PREFIX)) == 0;
}

esp_err_t claw_agent_last_reply(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    state_lock();
    strlcpy(out, s_last_reply ? s_last_reply : "", out_size);
    state_unlock();
    return ESP_OK;
}

esp_err_t claw_agent_status_json(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    claw_agent_settings_t cfg;
    if (claw_settings_get_agent(&cfg) != ESP_OK) {
        memset(&cfg, 0, sizeof(cfg));
    }

    state_lock();
    bool worker = s_worker_running;
    bool busy = worker || s_inflight_request != 0;
    uint32_t reply_seq = s_reply_seq;
    size_t reply_chars = s_last_reply ? strlen(s_last_reply) : 0;
    char error[AGENT_ERROR_MAX];
    strlcpy(error, s_last_error, sizeof(error));
    char task_id[AGENT_TASK_ID_SIZE];
    strlcpy(task_id, s_last_task_id, sizeof(task_id));
    char task_state[AGENT_STATE_MAX];
    strlcpy(task_state, s_last_task_state, sizeof(task_state));
    state_unlock();

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "compiled", true);
    cJSON_AddBoolToObject(root, "enabled", cfg.enabled);
    cJSON_AddBoolToObject(root, "configured", cfg.base_url[0] != '\0' &&
                          cfg.model[0] != '\0' &&
                          (strncmp(cfg.base_url, "http://", 7) == 0 ||
                           strncmp(cfg.base_url, "https://", 8) == 0));
    cJSON_AddBoolToObject(root, "api_key_set", cfg.api_key[0] != '\0');
#if CONFIG_TEMPLATE_CLAW_AGENT_MEMORY
    cJSON_AddBoolToObject(root, "memory", true);
#else
    cJSON_AddBoolToObject(root, "memory", false);
#endif
    cJSON_AddBoolToObject(root, "ready", s_agent_mgr_ready && s_core != NULL && cfg.enabled);
    cJSON_AddBoolToObject(root, "busy", busy);
    cJSON_AddBoolToObject(root, "worker", worker);
    cJSON_AddStringToObject(root, "backend", cfg.backend);
    const char *provider = strcmp(cfg.backend, "anthropic_compatible") == 0
                           ? "Claude"
                           : (strstr(cfg.base_url, "deepseek") != NULL
                              ? "DeepSeek" : "OpenAI");
    cJSON_AddStringToObject(root, "provider", provider);
    cJSON_AddStringToObject(root, "model", cfg.model);
    cJSON_AddStringToObject(root, "last_error", error);
    cJSON_AddNumberToObject(root, "reply_seq", (double)reply_seq);
    cJSON_AddNumberToObject(root, "reply_chars", (double)reply_chars);
    cJSON_AddStringToObject(root, "task_id", task_id);
    cJSON_AddStringToObject(root, "task_state", task_state);

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(out, text, out_size);
    cJSON_free(text);
    return ESP_OK;
}

#else /* !CONFIG_TEMPLATE_CLAW_AGENT */

esp_err_t claw_agent_init(void)
{
    return ESP_OK;
}

esp_err_t claw_agent_shutdown(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return ESP_OK;
}

void claw_agent_settings_changed(void)
{
}

esp_err_t claw_agent_prepare(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool claw_agent_is_ready(void)
{
    return false;
}

bool claw_agent_is_busy(void)
{
    return false;
}

esp_err_t claw_agent_status_json(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(out, "{\"compiled\":false}", out_size);
    return ESP_OK;
}

esp_err_t claw_agent_last_reply(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    return ESP_OK;
}

esp_err_t claw_agent_ask(const char *prompt, const char *origin,
                         uint32_t timeout_ms, char *out, size_t out_size)
{
    (void)prompt;
    (void)origin;
    (void)timeout_ms;
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(out, "Error: 本固件未启用本地 Agent (CONFIG_TEMPLATE_CLAW_AGENT)", out_size);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t claw_agent_start_task(const char *name, const char *prompt,
                                uint32_t timeout_ms, char *out, size_t out_size)
{
    (void)name;
    (void)prompt;
    (void)timeout_ms;
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(out, "Error: 本固件未启用本地 Agent (CONFIG_TEMPLATE_CLAW_AGENT)", out_size);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t claw_agent_stop_task(const char *task_id, uint32_t wait_ms,
                               char *out, size_t out_size)
{
    (void)task_id;
    (void)wait_ms;
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(out, "Error: not supported", out_size);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t claw_agent_get_task_output(const char *task_id, char *out, size_t out_size)
{
    (void)task_id;
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(out, "Error: not supported", out_size);
    return ESP_ERR_NOT_SUPPORTED;
}

bool claw_agent_is_agent_task_id(const char *task_id)
{
    (void)task_id;
    return false;
}

#endif /* CONFIG_TEMPLATE_CLAW_AGENT */
