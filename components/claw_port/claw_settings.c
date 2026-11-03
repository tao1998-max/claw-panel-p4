/*
 * ESP-Claw runtime settings. NVS namespace "claw" so the factory reset path
 * (nvs_flash_erase) clears them together with everything else.
 */
#include "claw_settings.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "claw_settings";

#define CLAW_NVS_NAMESPACE "claw"
#define KEY_LUA_AI         "lua_ai"
#define KEY_AGENT          "agent_en"
#define KEY_AGENT_BACKEND  "agent_backend"
#define KEY_AGENT_URL      "agent_base_url"
#define KEY_AGENT_MODEL    "agent_model"
#define KEY_AGENT_KEY      "agent_api_key"
#define KEY_AGENT_ENDPOINT "agent_endpoint"
#define KEY_AGENT_AUTH     "agent_auth"
#define KEY_AGENT_TOK_FIELD "agent_tok_field"
#define KEY_AGENT_REASONING "agent_reasoning"
#define KEY_AGENT_TIMEOUT  "agent_timeout"
#define KEY_AGENT_MAX_TOK  "agent_max_tok"
#define KEY_AGENT_IMG_MAX  "agent_img_max"
#define KEY_AGENT_TOOLS    "agent_tools"
#define KEY_AGENT_VISION   "agent_vision"
#define KEY_AGENT_IMG_URL  "agent_img_url"
#define KEY_PROV_DONE      "prov_done"

#define AGENT_BACKEND_DEFAULT "openai_compatible"

static bool read_bool(const char *key, bool default_value)
{
    nvs_handle_t handle;
    if (nvs_open(CLAW_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return default_value;
    }
    uint8_t value = default_value ? 1 : 0;
    esp_err_t err = nvs_get_u8(handle, key, &value);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return default_value;
    }
    return err == ESP_OK && value != 0;
}

static esp_err_t write_bool(const char *key, bool value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CLAW_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u8(handle, key, value ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void read_str(const char *key, const char *default_value,
                     char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';

    nvs_handle_t handle;
    if (nvs_open(CLAW_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        strlcpy(out, default_value ? default_value : "", out_size);
        return;
    }

    size_t len = out_size;
    esp_err_t err = nvs_get_str(handle, key, out, &len);
    nvs_close(handle);
    if (err != ESP_OK || len == 0) {
        strlcpy(out, default_value ? default_value : "", out_size);
    }
}

static uint32_t read_u32(const char *key, uint32_t default_value)
{
    nvs_handle_t handle;
    if (nvs_open(CLAW_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return default_value;
    }
    uint32_t value = default_value;
    esp_err_t err = nvs_get_u32(handle, key, &value);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return default_value;
    }
    return err == ESP_OK ? value : default_value;
}

static esp_err_t write_u32(const char *key, uint32_t value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CLAW_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_u32(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t write_str(const char *key, const char *value, size_t max_len)
{
    const char *text = value ? value : "";
    if (strlen(text) >= max_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CLAW_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_str(handle, key, text);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static bool backend_is_valid(const char *backend)
{
    return backend != NULL &&
           (strcmp(backend, "openai_compatible") == 0 ||
            strcmp(backend, "openai_responses") == 0 ||
            strcmp(backend, "anthropic_compatible") == 0);
}

bool claw_settings_lua_ai_enabled(void)
{
    return read_bool(KEY_LUA_AI, false);
}

esp_err_t claw_settings_set_lua_ai(bool enabled)
{
    ESP_LOGI(TAG, "AI Lua %s", enabled ? "enabled" : "disabled");
    return write_bool(KEY_LUA_AI, enabled);
}

bool claw_settings_agent_enabled(void)
{
    return read_bool(KEY_AGENT, false);
}

esp_err_t claw_settings_set_agent(bool enabled)
{
    ESP_LOGI(TAG, "local agent %s", enabled ? "enabled" : "disabled");
    return write_bool(KEY_AGENT, enabled);
}

esp_err_t claw_settings_get_agent(claw_agent_settings_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->enabled = read_bool(KEY_AGENT, false);
    read_str(KEY_AGENT_BACKEND, AGENT_BACKEND_DEFAULT,
             out->backend, sizeof(out->backend));
    read_str(KEY_AGENT_URL, "", out->base_url, sizeof(out->base_url));
    read_str(KEY_AGENT_MODEL, "", out->model, sizeof(out->model));
    read_str(KEY_AGENT_KEY, "", out->api_key, sizeof(out->api_key));
    read_str(KEY_AGENT_ENDPOINT, "", out->endpoint_provider,
             sizeof(out->endpoint_provider));
    read_str(KEY_AGENT_AUTH, "", out->auth_type, sizeof(out->auth_type));
    read_str(KEY_AGENT_TOK_FIELD, "", out->max_tokens_field,
             sizeof(out->max_tokens_field));
    read_str(KEY_AGENT_REASONING, "medium", out->reasoning_effort,
             sizeof(out->reasoning_effort));
    out->timeout_ms = read_u32(KEY_AGENT_TIMEOUT, 180000);
    out->max_tokens = read_u32(KEY_AGENT_MAX_TOK, 3072);
    out->image_max_bytes = read_u32(KEY_AGENT_IMG_MAX, 524288);
    out->supports_tools = read_bool(KEY_AGENT_TOOLS, true);
    out->supports_vision = read_bool(KEY_AGENT_VISION, false);
    out->image_remote_url_only = read_bool(KEY_AGENT_IMG_URL, true);
    if (!backend_is_valid(out->backend)) {
        strlcpy(out->backend, AGENT_BACKEND_DEFAULT, sizeof(out->backend));
    }
    return ESP_OK;
}

bool claw_settings_agent_configured(void)
{
    claw_agent_settings_t cfg;
    if (claw_settings_get_agent(&cfg) != ESP_OK) {
        return false;
    }
    if (cfg.base_url[0] == '\0' || cfg.model[0] == '\0') {
        return false;
    }
    return strncmp(cfg.base_url, "http://", 7) == 0 ||
           strncmp(cfg.base_url, "https://", 8) == 0;
}

esp_err_t claw_settings_set_agent_backend(const char *backend)
{
    if (!backend_is_valid(backend)) {
        return ESP_ERR_INVALID_ARG;
    }
    return write_str(KEY_AGENT_BACKEND, backend, CLAW_AGENT_BACKEND_MAX);
}

esp_err_t claw_settings_set_agent_base_url(const char *url)
{
    if (url == NULL) {
        url = "";
    }
    if (url[0] != '\0' &&
        strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return write_str(KEY_AGENT_URL, url, CLAW_AGENT_URL_MAX);
}

esp_err_t claw_settings_set_agent_model(const char *model)
{
    return write_str(KEY_AGENT_MODEL, model, CLAW_AGENT_MODEL_MAX);
}

esp_err_t claw_settings_set_agent_api_key(const char *key)
{
    return write_str(KEY_AGENT_KEY, key, CLAW_AGENT_KEY_MAX);
}

esp_err_t claw_settings_set_agent_llm_options(const claw_agent_settings_t *options)
{
    if (options == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = write_str(KEY_AGENT_ENDPOINT, options->endpoint_provider,
                              CLAW_AGENT_PROVIDER_MAX);
    if (err == ESP_OK) {
        err = write_str(KEY_AGENT_AUTH, options->auth_type, CLAW_AGENT_AUTH_MAX);
    }
    if (err == ESP_OK) {
        err = write_str(KEY_AGENT_TOK_FIELD, options->max_tokens_field,
                        CLAW_AGENT_TOKENS_FIELD_MAX);
    }
    if (err == ESP_OK) {
        err = write_str(KEY_AGENT_REASONING, options->reasoning_effort,
                        CLAW_AGENT_REASONING_MAX);
    }
    if (err == ESP_OK) {
        err = write_u32(KEY_AGENT_TIMEOUT,
                        options->timeout_ms ? options->timeout_ms : 180000);
    }
    if (err == ESP_OK) {
        err = write_u32(KEY_AGENT_MAX_TOK,
                        options->max_tokens ? options->max_tokens : 3072);
    }
    if (err == ESP_OK) {
        err = write_u32(KEY_AGENT_IMG_MAX,
                        options->image_max_bytes ? options->image_max_bytes : 524288);
    }
    if (err == ESP_OK) {
        err = write_bool(KEY_AGENT_TOOLS, options->supports_tools);
    }
    if (err == ESP_OK) {
        err = write_bool(KEY_AGENT_VISION, options->supports_vision);
    }
    if (err == ESP_OK) {
        err = write_bool(KEY_AGENT_IMG_URL, options->image_remote_url_only);
    }
    return err;
}

bool claw_settings_provision_done(void)
{
    return read_bool(KEY_PROV_DONE, false);
}

esp_err_t claw_settings_set_provision_done(bool done)
{
    return write_bool(KEY_PROV_DONE, done);
}
