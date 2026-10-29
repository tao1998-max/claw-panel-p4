/*
 * Runtime API used by the XiaoZhi MCP bridge and the Claw page.
 *
 * Every function is fast-returning from the caller's point of view:
 *   - run_lua blocks at most timeout_ms (clamped to the Kconfig sync limit)
 *   - start_task returns immediately with a job id
 *   - list/get/stop/status only touch the in-memory job tables
 *
 * Text outputs are human readable; callers forward them to the LLM/UI.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "claw_tasks.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register cleanup hooks (temporary AI script files). Call once at init. */
esp_err_t claw_api_init(void);

/** One-line JSON status: Lua switch, jobs, schedules, storage, agent. */
esp_err_t claw_api_status_json(char *out, size_t out_size);

/** Run inline code synchronously. */
esp_err_t claw_api_run_lua(const char *code, const char *args_json,
                           uint32_t timeout_ms, char *out, size_t out_size);

/**
 * Allow start_task(path=...) to run a Lua script from an extra absolute root
 * such as the SD-card apps directory. Scripts must sit under `root` and end
 * in .lua; up to two extra roots are kept. Register before any task starts
 * (claw_port_init() does this for the SD apps root).
 */
esp_err_t claw_api_add_script_root(const char *root);

/**
 * Start a background job from inline code or an existing script path.
 * At least one of code/path must be set; code wins when both are given.
 */
esp_err_t claw_api_start_task(const char *name, const char *code, const char *path,
                              const char *args_json, uint32_t timeout_ms,
                              char *out, size_t out_size);

/** Same, but records the caller and enforces per-origin quotas. */
esp_err_t claw_api_start_task_ex(const char *name, const char *code, const char *path,
                                 const char *args_json, uint32_t timeout_ms,
                                 claw_task_origin_t origin,
                                 char *out, size_t out_size);

esp_err_t claw_api_list_tasks(const char *state, char *out, size_t out_size);
esp_err_t claw_api_get_task(const char *id, char *out, size_t out_size);
esp_err_t claw_api_stop_task(const char *id, uint32_t wait_ms, char *out, size_t out_size);

/** Delete a finished task record. */
esp_err_t claw_api_remove_task(const char *id, char *out, size_t out_size);
/** Delete every finished task record. */
esp_err_t claw_api_clear_finished_tasks(char *out, size_t out_size);

/** Create data/scripts/<name>.lua with a small template. */
esp_err_t claw_api_create_script(const char *name, char *out, size_t out_size);
/** Delete data/scripts/<name>.lua. */
esp_err_t claw_api_delete_script(const char *name, char *out, size_t out_size);

/**
 * Add a schedule that runs a Lua script.
 * kind: once | interval | cron
 * when: epoch seconds (once), seconds (interval) or a cron expression.
 */
esp_err_t claw_api_add_schedule(const char *id, const char *kind, const char *when,
                                const char *script, const char *args_json,
                                char *out, size_t out_size);

/** JSON array of user scripts: {name, path, size}. */
esp_err_t claw_api_list_scripts_json(char *out, size_t out_size);

/** True when the same script already has a queued/running job (UI guard). */
bool claw_api_is_script_active(const char *path);

/** JSON array of skill templates: {id, title, path, source} (path may be empty). */
esp_err_t claw_api_list_skills_json(char *out, size_t out_size);

esp_err_t claw_api_list_schedules(char *out, size_t out_size);
esp_err_t claw_api_remove_schedule(const char *id, char *out, size_t out_size);
esp_err_t claw_api_set_schedule_enabled(const char *id, bool enabled,
                                        char *out, size_t out_size);

/* ---------- optional local agent (returns not-supported when compiled out) ---------- */

/** JSON: compiled/enabled/ready/busy/backend/model/last_error/reply_seq/task. */
esp_err_t claw_api_agent_status_json(char *out, size_t out_size);

/** Raw text of the most recent final answer. */
esp_err_t claw_api_agent_last_reply(char *out, size_t out_size);

/** Persist the NVS switch, validating the configuration when enabling. */
esp_err_t claw_api_agent_set_enabled(bool enabled, char *out, size_t out_size);

/** Notify the agent that its NVS configuration changed (rebuilds lazily). */
void claw_api_agent_settings_changed(void);

/** Synchronous ask, bounded by timeout_ms (clamped by the agent). */
esp_err_t claw_api_agent_ask(const char *prompt, const char *origin,
                             uint32_t timeout_ms, char *out, size_t out_size);

/** Background run; returns {"task_id":...}. Read it with claw_api_get_task. */
esp_err_t claw_api_agent_start_task(const char *name, const char *prompt,
                                    uint32_t timeout_ms, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
