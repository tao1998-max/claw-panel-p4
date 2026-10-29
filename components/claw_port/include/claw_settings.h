/*
 * ESP-Claw runtime settings (NVS namespace "claw").
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** AI-generated Lua execution switch. Default: OFF. */
bool claw_settings_lua_ai_enabled(void);
esp_err_t claw_settings_set_lua_ai(bool enabled);

/** Local agent switch (only meaningful when CONFIG_TEMPLATE_CLAW_AGENT=y). */
bool claw_settings_agent_enabled(void);
esp_err_t claw_settings_set_agent(bool enabled);

/* ---------- local agent configuration (NVS namespace "claw") ---------- */

#define CLAW_AGENT_BACKEND_MAX 24
#define CLAW_AGENT_URL_MAX     192
#define CLAW_AGENT_MODEL_MAX   64
#define CLAW_AGENT_KEY_MAX     192
#define CLAW_AGENT_PROVIDER_MAX 32
#define CLAW_AGENT_AUTH_MAX    24
#define CLAW_AGENT_TOKENS_FIELD_MAX 32
#define CLAW_AGENT_REASONING_MAX 16

typedef struct {
    bool enabled;
    char backend[CLAW_AGENT_BACKEND_MAX];  /* openai_compatible | anthropic_compatible */
    char base_url[CLAW_AGENT_URL_MAX];     /* e.g. https://api.openai.com/v1 */
    char model[CLAW_AGENT_MODEL_MAX];
    char api_key[CLAW_AGENT_KEY_MAX];      /* empty = no auth (local endpoint) */
    /* Advanced LLM options exposed by the official Web UI. Empty strings mean
     * "use the backend default"; auth_type empty means derive from backend. */
    char endpoint_provider[CLAW_AGENT_PROVIDER_MAX];
    char auth_type[CLAW_AGENT_AUTH_MAX];
    char max_tokens_field[CLAW_AGENT_TOKENS_FIELD_MAX];
    char reasoning_effort[CLAW_AGENT_REASONING_MAX];
    uint32_t timeout_ms;
    uint32_t max_tokens;
    uint32_t image_max_bytes;
    bool supports_tools;
    bool supports_vision;
    bool image_remote_url_only;
} claw_agent_settings_t;

/** Load the whole agent configuration, filling defaults for missing keys. */
esp_err_t claw_settings_get_agent(claw_agent_settings_t *out);

/** True when base URL and model are valid enough to start the agent. */
bool claw_settings_agent_configured(void);

esp_err_t claw_settings_set_agent_backend(const char *backend);
esp_err_t claw_settings_set_agent_base_url(const char *url);
esp_err_t claw_settings_set_agent_model(const char *model);
esp_err_t claw_settings_set_agent_api_key(const char *key);

/** Persist the advanced LLM options (timeout/tokens/vision/tools/...). */
esp_err_t claw_settings_set_agent_llm_options(const claw_agent_settings_t *options);

/** One-shot build-time provisioning marker (prevents re-enabling a user-off agent). */
bool claw_settings_provision_done(void);
esp_err_t claw_settings_set_provision_done(bool done);

#ifdef __cplusplus
}
#endif
