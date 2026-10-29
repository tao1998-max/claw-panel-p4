/*
 * Product-side app_config compatibility for the official ESP-Claw Web UI.
 *
 * The upstream component is backed by app_claw/app_config (Wi-Fi manager,
 * QQ/Feishu/Telegram/WeChat, capability and Lua module toggles). This project
 * keeps its own settings and Claw runtime, so only the Web UI-visible fields
 * are mapped here.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CONFIG_STR_LEN      320
#define APP_CONFIG_TIMEZONE_LEN 32

typedef struct {
    char wifi_ssid[APP_CONFIG_STR_LEN];
    char wifi_password[APP_CONFIG_STR_LEN];
    char llm_api_key[APP_CONFIG_STR_LEN];
    char llm_backend_type[32];
    char llm_endpoint_provider[32];
    char llm_model[64];
    char llm_base_url[APP_CONFIG_STR_LEN];
    char llm_auth_type[32];
    char llm_timeout_ms[16];
    char llm_max_tokens[16];
    char llm_default_image_max_bytes[16];
    char llm_max_tokens_field[32];
    char llm_reasoning_effort[16];
    char llm_supports_tools[8];
    char llm_supports_vision[8];
    char llm_image_remote_url_only[8];
    char qq_app_id[32];
    char qq_app_secret[APP_CONFIG_STR_LEN];
    char qq_msg_type[8];
    char feishu_app_id[64];
    char feishu_app_secret[APP_CONFIG_STR_LEN];
    char tg_bot_token[APP_CONFIG_STR_LEN];
    char wechat_token[APP_CONFIG_STR_LEN];
    char wechat_base_url[APP_CONFIG_STR_LEN];
    char wechat_cdn_base_url[APP_CONFIG_STR_LEN];
    char wechat_account_id[32];
    char search_provider[16];
    char search_bocha_key[APP_CONFIG_STR_LEN];
    char search_brave_key[APP_CONFIG_STR_LEN];
    char search_tavily_key[APP_CONFIG_STR_LEN];
    char search_http_allowlist[APP_CONFIG_STR_LEN];
    char llm_visible_cap_groups[APP_CONFIG_STR_LEN];
    char time_timezone[APP_CONFIG_TIMEZONE_LEN];
} app_config_t;

esp_err_t app_config_load(app_config_t *config);
esp_err_t app_config_save(const app_config_t *config);
esp_err_t app_config_validate_wifi(const app_config_t *config, const char **message);

/* Apply the TZ environment before Claw starts; call at boot and after save. */
esp_err_t app_config_apply_timezone(const app_config_t *config);
/* Push persisted IM credentials into cap_im_platform and start its gateways.
 * Call after claw_port_init() so the IM groups/state exist. */
esp_err_t app_config_apply_im(const app_config_t *config);

#ifdef __cplusplus
}
#endif
