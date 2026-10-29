/*
 * Claw task registry: one bounded table shared by Lua jobs, schedules and the
 * optional agent. It records origin (who started it), state, timing and the
 * latest progress; the Claw page renders a JSON snapshot.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLAW_TASK_ORIGIN_AI = 0,   /* XiaoZhi cloud through MCP */
    CLAW_TASK_ORIGIN_USER,     /* panel / user script */
    CLAW_TASK_ORIGIN_SCHEDULED,/* scheduler */
    CLAW_TASK_ORIGIN_AGENT,    /* local agent (optional) */
    CLAW_TASK_ORIGIN_COUNT
} claw_task_origin_t;

const char *claw_task_origin_name(claw_task_origin_t origin);
bool claw_task_origin_parse(const char *name, claw_task_origin_t *out);

/** Register the cap_lua job event hook (idempotent). */
esp_err_t claw_task_registry_init(void);

/** Remember the origin of a job right after submitting it. */
void claw_task_note_submit(const char *job_id, const char *name, const char *path,
                           claw_task_origin_t origin);

/** Active (queued/running) jobs for one origin; used to enforce quotas. */
size_t claw_task_active_count(claw_task_origin_t origin);

/** Called from Lua/Agent tasks; never touches LVGL. */
void claw_task_report_progress(const char *job_id, int percent, const char *message);

/** Read the last progress reported by a job (or the most recent one). */
esp_err_t claw_task_get_progress(const char *job_id, int *percent,
                                 char *message, size_t message_size);

/** JSON array of task records, newest first. */
esp_err_t claw_task_registry_snapshot_json(char *out, size_t out_size);

/** Update a record's origin (schedules/agent may start jobs indirectly). */
void claw_task_set_origin(const char *job_id, claw_task_origin_t origin);

/** Remove a finished task record. Active records are refused. */
bool claw_task_remove(const char *job_id);

/** Remove all finished task records; returns how many were removed. */
size_t claw_task_clear_finished(void);

/* ---------- local agent task records (CLI-less worker, no Lua job) ---------- */

/** Create the record for a background agent run and mark it queued. */
void claw_task_agent_begin(const char *task_id, const char *name, const char *prompt);

/** Update state/progress. state uses Lua job names: running/done/failed/timeout/stopped. */
void claw_task_agent_update(const char *task_id, const char *state,
                            const char *message, int progress);

/** Terminal update (records the end time). */
void claw_task_agent_finish(const char *task_id, const char *state,
                            const char *message);

#ifdef __cplusplus
}
#endif
