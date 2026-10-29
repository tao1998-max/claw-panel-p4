#include "app_config.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cap_http_request.h"
#include "cap_im_feishu.h"
#include "cap_im_qq.h"
#include "cap_im_tg.h"
#include "cap_im_wechat.h"
#include "cap_web_search.h"
#include "claw_agent.h"
#include "claw_cap.h"
#include "claw_settings.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs.h"

static const char *TAG = "web_config";
#define WEBUI_NVS_NS "webui"

static const char *const s_default_visible_groups =
    "cap_claw,cap_files,cap_lua,cap_scheduler,cap_router_mgr,cap_skill,"
    "cap_system,cap_llm_inspect,cap_http_request,cap_web_search,"
    "cap_agent_mgr,claw_memory";

static void nvs_read_str(const char *key, char *out, size_t out_size, const char *fallback)
{
    nvs_handle_t handle;
    size_t size = out_size;
    if (nvs_open(WEBUI_NVS_NS, NVS_READONLY, &handle) == ESP_OK) {
        esp_err_t err = nvs_get_str(handle, key, out, &size);
        nvs_close(handle);
        if (err == ESP_OK) {
            return;
        }
    }
    strlcpy(out, fallback ? fallback : "", out_size);
}

static esp_err_t nvs_write_str(const char *key, const char *value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WEBUI_NVS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, key, value ? value : "");
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t app_config_load(app_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(config, 0, sizeof(*config));

    wifi_config_t wifi_cfg = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) == ESP_OK) {
        strlcpy(config->wifi_ssid, (const char *)wifi_cfg.sta.ssid,
                sizeof(config->wifi_ssid));
    }
    claw_agent_settings_t agent = {0};
    if (claw_settings_get_agent(&agent) == ESP_OK) {
        strlcpy(config->llm_api_key, agent.api_key, sizeof(config->llm_api_key));
        strlcpy(config->llm_backend_type, agent.backend, sizeof(config->llm_backend_type));
        strlcpy(config->llm_model, agent.model, sizeof(config->llm_model));
        strlcpy(config->llm_base_url, agent.base_url, sizeof(config->llm_base_url));
        if (agent.auth_type[0]) {
            strlcpy(config->llm_auth_type, agent.auth_type, sizeof(config->llm_auth_type));
        } else {
            strlcpy(config->llm_auth_type,
                    agent.api_key[0] == '\0'
                        ? "none"
                        : (strcmp(agent.backend, "anthropic_compatible") == 0
                               ? "api-key" : "bearer"),
                    sizeof(config->llm_auth_type));
        }
        strlcpy(config->llm_endpoint_provider,
                agent.endpoint_provider[0] ? agent.endpoint_provider : "static",
                sizeof(config->llm_endpoint_provider));
        snprintf(config->llm_timeout_ms, sizeof(config->llm_timeout_ms), "%lu",
                 (unsigned long)(agent.timeout_ms ? agent.timeout_ms : 60000));
        snprintf(config->llm_max_tokens, sizeof(config->llm_max_tokens), "%lu",
                 (unsigned long)(agent.max_tokens ? agent.max_tokens : 3072));
        snprintf(config->llm_default_image_max_bytes,
                 sizeof(config->llm_default_image_max_bytes), "%lu",
                 (unsigned long)(agent.image_max_bytes ? agent.image_max_bytes : 524288));
        strlcpy(config->llm_max_tokens_field,
                agent.max_tokens_field[0] ? agent.max_tokens_field : "max_tokens",
                sizeof(config->llm_max_tokens_field));
        strlcpy(config->llm_reasoning_effort,
                agent.reasoning_effort[0] ? agent.reasoning_effort : "medium",
                sizeof(config->llm_reasoning_effort));
        strlcpy(config->llm_supports_tools,
                agent.supports_tools ? "true" : "false",
                sizeof(config->llm_supports_tools));
        strlcpy(config->llm_supports_vision,
                agent.supports_vision ? "true" : "false",
                sizeof(config->llm_supports_vision));
        strlcpy(config->llm_image_remote_url_only,
                agent.image_remote_url_only ? "true" : "false",
                sizeof(config->llm_image_remote_url_only));
    } else {
        strlcpy(config->llm_auth_type, "none", sizeof(config->llm_auth_type));
        strlcpy(config->llm_endpoint_provider, "static",
                sizeof(config->llm_endpoint_provider));
        strlcpy(config->llm_timeout_ms, "60000", sizeof(config->llm_timeout_ms));
        strlcpy(config->llm_max_tokens, "3072", sizeof(config->llm_max_tokens));
        strlcpy(config->llm_default_image_max_bytes, "524288",
                sizeof(config->llm_default_image_max_bytes));
        strlcpy(config->llm_max_tokens_field, "max_tokens",
                sizeof(config->llm_max_tokens_field));
        strlcpy(config->llm_reasoning_effort, "medium",
                sizeof(config->llm_reasoning_effort));
        strlcpy(config->llm_supports_tools, "true", sizeof(config->llm_supports_tools));
        strlcpy(config->llm_supports_vision, "false", sizeof(config->llm_supports_vision));
        strlcpy(config->llm_image_remote_url_only, "true",
                sizeof(config->llm_image_remote_url_only));
    }

    nvs_read_str("search_provider", config->search_provider,
                 sizeof(config->search_provider), "bocha");
    nvs_read_str("search_bocha_key", config->search_bocha_key,
                 sizeof(config->search_bocha_key), "");
    nvs_read_str("search_brave_key", config->search_brave_key,
                 sizeof(config->search_brave_key), "");
    nvs_read_str("search_tavily_key", config->search_tavily_key,
                 sizeof(config->search_tavily_key), "");
    nvs_read_str("search_allowlist", config->search_http_allowlist,
                 sizeof(config->search_http_allowlist), "*");

    nvs_read_str("qq_app_id", config->qq_app_id, sizeof(config->qq_app_id), "");
    nvs_read_str("qq_app_secret", config->qq_app_secret,
                 sizeof(config->qq_app_secret), "");
    nvs_read_str("qq_msg_type", config->qq_msg_type,
                 sizeof(config->qq_msg_type), "0");
    nvs_read_str("feishu_app_id", config->feishu_app_id,
                 sizeof(config->feishu_app_id), "");
    nvs_read_str("feishu_app_secret", config->feishu_app_secret,
                 sizeof(config->feishu_app_secret), "");
    nvs_read_str("tg_bot_token", config->tg_bot_token,
                 sizeof(config->tg_bot_token), "");
    nvs_read_str("wechat_token", config->wechat_token,
                 sizeof(config->wechat_token), "");
    nvs_read_str("wechat_base_url", config->wechat_base_url,
                 sizeof(config->wechat_base_url),
                 "https://ilinkai.weixin.qq.com");
    nvs_read_str("wechat_cdn_base_url", config->wechat_cdn_base_url,
                 sizeof(config->wechat_cdn_base_url),
                 "https://novac2c.cdn.weixin.qq.com/c2c");
    nvs_read_str("wechat_account_id", config->wechat_account_id,
                 sizeof(config->wechat_account_id), "");

    nvs_read_str("llm_visible_cap_groups", config->llm_visible_cap_groups,
                 sizeof(config->llm_visible_cap_groups),
                 s_default_visible_groups);
    nvs_read_str("time_timezone", config->time_timezone,
                 sizeof(config->time_timezone), "CST-8");
    return ESP_OK;
}

static bool secret_is_masked(const char *value)
{
    return value && strchr(value, '*') != NULL;
}

static uint32_t config_u32(const char *text, uint32_t fallback)
{
    if (text == NULL || text[0] == '\0') {
        return fallback;
    }
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (end == text || (end && *end != '\0') || value == 0 ||
        value > UINT32_MAX) {
        return fallback;
    }
    return (uint32_t)value;
}

static bool config_bool(const char *text, bool fallback)
{
    if (text == NULL || text[0] == '\0') {
        return fallback;
    }
    if (strcmp(text, "true") == 0 || strcmp(text, "1") == 0 ||
        strcmp(text, "yes") == 0 || strcmp(text, "on") == 0) {
        return true;
    }
    if (strcmp(text, "false") == 0 || strcmp(text, "0") == 0 ||
        strcmp(text, "no") == 0 || strcmp(text, "off") == 0) {
        return false;
    }
    return fallback;
}

static bool reasoning_effort_valid(const char *text)
{
    return text == NULL || text[0] == '\0' ||
           strcmp(text, "none") == 0 || strcmp(text, "low") == 0 ||
           strcmp(text, "medium") == 0 || strcmp(text, "high") == 0 ||
           strcmp(text, "xhigh") == 0 || strcmp(text, "max") == 0;
}

static void apply_visible_groups(const char *csv)
{
    if (!csv || !csv[0]) {
        return;
    }
    char copy[APP_CONFIG_STR_LEN];
    strlcpy(copy, csv, sizeof(copy));
    const char *groups[32];
    size_t count = 0;
    char *save = NULL;
    for (char *token = strtok_r(copy, ",", &save);
         token && count < sizeof(groups) / sizeof(groups[0]);
         token = strtok_r(NULL, ",", &save)) {
        while (*token && isspace((unsigned char)*token)) {
            token++;
        }
        size_t len = strlen(token);
        while (len > 0 && isspace((unsigned char)token[len - 1])) {
            token[--len] = '\0';
        }
        if (*token) {
            groups[count++] = token;
        }
    }
    if (count > 0) {
        (void)claw_cap_set_llm_visible_groups(groups, count);
    }
}

esp_err_t app_config_save(const app_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (config->llm_backend_type[0]) {
        esp_err_t err = claw_settings_set_agent_backend(config->llm_backend_type);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (config->llm_base_url[0]) {
        esp_err_t err = claw_settings_set_agent_base_url(config->llm_base_url);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (config->llm_model[0]) {
        esp_err_t err = claw_settings_set_agent_model(config->llm_model);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (config->llm_api_key[0] && !secret_is_masked(config->llm_api_key)) {
        esp_err_t err = claw_settings_set_agent_api_key(config->llm_api_key);
        if (err != ESP_OK) {
            return err;
        }
    }

    claw_agent_settings_t llm_options = {0};
    strlcpy(llm_options.endpoint_provider, config->llm_endpoint_provider,
            sizeof(llm_options.endpoint_provider));
    strlcpy(llm_options.auth_type, config->llm_auth_type,
            sizeof(llm_options.auth_type));
    strlcpy(llm_options.max_tokens_field, config->llm_max_tokens_field,
            sizeof(llm_options.max_tokens_field));
    strlcpy(llm_options.reasoning_effort,
            reasoning_effort_valid(config->llm_reasoning_effort)
                ? config->llm_reasoning_effort : "medium",
            sizeof(llm_options.reasoning_effort));
    llm_options.timeout_ms = config_u32(config->llm_timeout_ms, 60000);
    llm_options.max_tokens = config_u32(config->llm_max_tokens, 3072);
    llm_options.image_max_bytes =
        config_u32(config->llm_default_image_max_bytes, 524288);
    llm_options.supports_tools = config_bool(config->llm_supports_tools, true);
    llm_options.supports_vision = config_bool(config->llm_supports_vision, false);
    llm_options.image_remote_url_only =
        config_bool(config->llm_image_remote_url_only, true);
    esp_err_t options_err = claw_settings_set_agent_llm_options(&llm_options);
    if (options_err != ESP_OK) {
        return options_err;
    }
    claw_agent_settings_changed();

    wifi_config_t current_wifi = {0};
    (void)esp_wifi_get_config(WIFI_IF_STA, &current_wifi);
    bool wifi_changed = config->wifi_ssid[0] &&
                        strcmp((const char *)current_wifi.sta.ssid, config->wifi_ssid) != 0;
    if (config->wifi_password[0] && !secret_is_masked(config->wifi_password)) {
        wifi_changed = true;
    }
    if (wifi_changed) {
        wifi_config_t next_wifi = {0};
        strlcpy((char *)next_wifi.sta.ssid, config->wifi_ssid,
                sizeof(next_wifi.sta.ssid));
        strlcpy((char *)next_wifi.sta.password, config->wifi_password,
                sizeof(next_wifi.sta.password));
        next_wifi.sta.threshold.authmode =
            config->wifi_password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        if (esp_wifi_set_config(WIFI_IF_STA, &next_wifi) == ESP_OK) {
            esp_wifi_disconnect();
            (void)esp_wifi_connect();
        }
    }

    if (config->search_provider[0]) {
        esp_err_t err = nvs_write_str("search_provider", config->search_provider);
        if (err == ESP_OK) {
            (void)cap_web_search_set_provider(config->search_provider);
        }
    }
    if (!secret_is_masked(config->search_bocha_key)) {
        (void)nvs_write_str("search_bocha_key", config->search_bocha_key);
        (void)cap_web_search_set_bocha_key(config->search_bocha_key);
    }
    if (!secret_is_masked(config->search_brave_key)) {
        (void)nvs_write_str("search_brave_key", config->search_brave_key);
        (void)cap_web_search_set_brave_key(config->search_brave_key);
    }
    if (!secret_is_masked(config->search_tavily_key)) {
        (void)nvs_write_str("search_tavily_key", config->search_tavily_key);
        (void)cap_web_search_set_tavily_key(config->search_tavily_key);
    }
    if (config->search_http_allowlist[0]) {
        (void)nvs_write_str("search_allowlist", config->search_http_allowlist);
        (void)cap_http_request_set_allowlist(config->search_http_allowlist);
    }

    /* IM credentials: persist all non-secret fields, keep the old secret when
     * the Web UI sends the masked value back. */
    (void)nvs_write_str("qq_app_id", config->qq_app_id);
    if (!secret_is_masked(config->qq_app_secret)) {
        (void)nvs_write_str("qq_app_secret", config->qq_app_secret);
    }
    (void)nvs_write_str("qq_msg_type", config->qq_msg_type);
    (void)nvs_write_str("feishu_app_id", config->feishu_app_id);
    if (!secret_is_masked(config->feishu_app_secret)) {
        (void)nvs_write_str("feishu_app_secret", config->feishu_app_secret);
    }
    if (!secret_is_masked(config->tg_bot_token)) {
        (void)nvs_write_str("tg_bot_token", config->tg_bot_token);
    }
    if (!secret_is_masked(config->wechat_token)) {
        (void)nvs_write_str("wechat_token", config->wechat_token);
    }
    (void)nvs_write_str("wechat_base_url", config->wechat_base_url);
    (void)nvs_write_str("wechat_cdn_base_url", config->wechat_cdn_base_url);
    (void)nvs_write_str("wechat_account_id", config->wechat_account_id);

    if (config->llm_visible_cap_groups[0]) {
        (void)nvs_write_str("llm_visible_cap_groups",
                            config->llm_visible_cap_groups);
    }
    if (config->time_timezone[0]) {
        (void)nvs_write_str("time_timezone", config->time_timezone);
        (void)app_config_apply_timezone(config);
    }

    /* The Web UI echoes secrets as "****". Replace masked values with the
     * stored ones before starting the IM gateways, otherwise the gateway
     * would receive the mask as its credential. Keep the copy off the HTTP
     * task stack: app_config_t is ~5 KB. */
    app_config_t *effective = heap_caps_calloc(
        1, sizeof(*effective), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (effective != NULL) {
        *effective = *config;
        if (secret_is_masked(effective->qq_app_secret)) {
            nvs_read_str("qq_app_secret", effective->qq_app_secret,
                         sizeof(effective->qq_app_secret), "");
        }
        if (secret_is_masked(effective->feishu_app_secret)) {
            nvs_read_str("feishu_app_secret", effective->feishu_app_secret,
                         sizeof(effective->feishu_app_secret), "");
        }
        if (secret_is_masked(effective->tg_bot_token)) {
            nvs_read_str("tg_bot_token", effective->tg_bot_token,
                         sizeof(effective->tg_bot_token), "");
        }
        if (secret_is_masked(effective->wechat_token)) {
            nvs_read_str("wechat_token", effective->wechat_token,
                         sizeof(effective->wechat_token), "");
        }
        (void)app_config_apply_im(effective);
        heap_caps_free(effective);
    }

    apply_visible_groups(config->llm_visible_cap_groups);

    ESP_LOGI(TAG, "saved Web config ssid=%s model=%s provider=%s allowlist=%s",
             config->wifi_ssid, config->llm_model, config->search_provider,
             config->search_http_allowlist);
    return ESP_OK;
}

esp_err_t app_config_apply_timezone(const app_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *tz = config->time_timezone[0] ? config->time_timezone : "CST-8";
    setenv("TZ", tz, 1);
    tzset();
    ESP_LOGI(TAG, "timezone=%s", tz);
    return ESP_OK;
}

esp_err_t app_config_apply_im(const app_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t first_err = ESP_OK;
    esp_err_t err = ESP_OK;

    if (config->feishu_app_id[0] && config->feishu_app_secret[0]) {
        err = cap_im_feishu_set_credentials(config->feishu_app_id,
                                            config->feishu_app_secret);
        if (err == ESP_OK) {
            err = cap_im_feishu_start();
        }
        ESP_LOGI(TAG, "Feishu IM %s", err == ESP_OK ? "started" : "failed");
    } else {
        (void)cap_im_feishu_stop();
    }
    if (err != ESP_OK && first_err == ESP_OK) first_err = err;

    if (config->qq_app_id[0] && config->qq_app_secret[0]) {
        err = cap_im_qq_set_credentials(config->qq_app_id,
                                        config->qq_app_secret);
        if (err == ESP_OK) {
            err = cap_im_qq_start();
        }
        ESP_LOGI(TAG, "QQ IM %s", err == ESP_OK ? "started" : "failed");
    } else {
        (void)cap_im_qq_stop();
    }
    if (err != ESP_OK && first_err == ESP_OK) first_err = err;

    if (config->tg_bot_token[0]) {
        err = cap_im_tg_set_token(config->tg_bot_token);
        if (err == ESP_OK) {
            err = cap_im_tg_start();
        }
        ESP_LOGI(TAG, "Telegram IM %s", err == ESP_OK ? "started" : "failed");
    } else {
        (void)cap_im_tg_stop();
    }
    if (err != ESP_OK && first_err == ESP_OK) first_err = err;

    if (config->wechat_token[0] && config->wechat_base_url[0]) {
        cap_im_wechat_client_config_t wechat = {
            .token = config->wechat_token,
            .base_url = config->wechat_base_url,
            .cdn_base_url = config->wechat_cdn_base_url,
            .account_id = config->wechat_account_id,
        };
        err = cap_im_wechat_set_client_config(&wechat);
        if (err == ESP_OK) {
            err = cap_im_wechat_start();
        }
        ESP_LOGI(TAG, "WeChat IM %s", err == ESP_OK ? "started" : "failed");
    } else {
        (void)cap_im_wechat_stop();
    }
    if (err != ESP_OK && first_err == ESP_OK) first_err = err;

    return first_err;
}

esp_err_t app_config_validate_wifi(const app_config_t *config, const char **message)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->wifi_password[0] && !config->wifi_ssid[0]) {
        if (message) *message = "Wi-Fi SSID is required when a password is set";
        return ESP_ERR_INVALID_ARG;
    }
    if (config->wifi_password[0] && strlen(config->wifi_password) < 8) {
        if (message) *message = "Wi-Fi password must be at least 8 characters";
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}
