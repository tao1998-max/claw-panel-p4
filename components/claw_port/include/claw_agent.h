/*
 * Optional local ESP-Claw Agent.
 *
 * The agent is a second, self-contained brain: it talks to an
 * OpenAI-compatible or Anthropic-compatible endpoint with its own API key and
 * uses ESP-Claw capabilities as tools. It is compiled only when
 * CONFIG_TEMPLATE_CLAW_AGENT=y, starts lazily on the first request, and stays
 * completely idle (no TLS, no tasks) while the NVS switch is off.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register the Claw-specific agent tools and state. Call once at boot. */
esp_err_t claw_agent_init(void);

/** Stop the worker and destroy the LLM core. Safe to call repeatedly. */
esp_err_t claw_agent_shutdown(uint32_t timeout_ms);

/** Re-read NVS after a settings change; tears down an idle core. */
void claw_agent_settings_changed(void);

/**
 * Bring up the official claw_agent_mgr/root core when the Agent switch is on
 * and the endpoint/model are configured. This mirrors upstream app_claw_start()
 * and makes the session catalog/history usable before the first request. Safe
 * to call repeatedly.
 */
esp_err_t claw_agent_prepare(void);

bool claw_agent_is_ready(void);
bool claw_agent_is_busy(void);

/** One-line JSON status for the page/MCP. */
esp_err_t claw_agent_status_json(char *out, size_t out_size);

/** Copy the most recent final answer (raw text, not JSON). */
esp_err_t claw_agent_last_reply(char *out, size_t out_size);

/** Snapshot the assistant text currently streaming for a session. */
esp_err_t claw_agent_stream_snapshot(const char *session_id, char *out,
                                     size_t out_size, uint32_t *out_rev,
                                     bool *out_active);

/**
 * Ask synchronously. Blocks at most timeout_ms. `origin` is only recorded in
 * logs/session metadata ("xiaozhi" or "panel").
 */
esp_err_t claw_agent_ask(const char *prompt, const char *origin,
                         uint32_t timeout_ms, char *out, size_t out_size);

/** Start a background agent run; returns a JSON receipt with task_id. */
esp_err_t claw_agent_start_task(const char *name, const char *prompt,
                                uint32_t timeout_ms, char *out, size_t out_size);

/** Cancel/wait for the background run identified by task_id. */
esp_err_t claw_agent_stop_task(const char *task_id, uint32_t wait_ms,
                               char *out, size_t out_size);

/** Final output of the most recent agent task, or ESP_ERR_NOT_FOUND. */
esp_err_t claw_agent_get_task_output(const char *task_id, char *out, size_t out_size);

/** True for ids created by claw_agent_start_task ("agent-..."). */
bool claw_agent_is_agent_task_id(const char *task_id);

#ifdef __cplusplus
}
#endif
