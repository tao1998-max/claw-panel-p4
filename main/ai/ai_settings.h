#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 小智运行时配置，独立 NVS 命名空间 ai，不污染 core/settings 的 sys。 */
esp_err_t ai_settings_init(void);

bool ai_settings_enabled(void);
void ai_settings_set_enabled(bool enabled);

bool ai_settings_wake_word(void);
void ai_settings_set_wake_word(bool enabled);

bool ai_settings_muted(void);
void ai_settings_set_muted(bool muted);

const char *ai_settings_ota_url(void);
esp_err_t ai_settings_set_ota_url(const char *url);

const char *ai_settings_ws_url(void);
const char *ai_settings_token(void);
esp_err_t ai_settings_set_direct_ws(const char *url, const char *token);

/* 只清激活配置（websocket namespace），保留 ai 配置和 Wi-Fi 凭据。 */
esp_err_t ai_settings_clear_activation(void);

#ifdef __cplusplus
}
#endif
