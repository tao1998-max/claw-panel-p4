#include "ai.h"

#include "sdkconfig.h"

#if !CONFIG_TEMPLATE_ENABLE_XIAOZHI

esp_err_t ai_early_init(void) { return ESP_OK; }
esp_err_t ai_start(void) { return ESP_ERR_NOT_SUPPORTED; }
void ai_stop(void) { }
bool ai_available(void) { return false; }
bool ai_enabled(void) { return false; }
void ai_set_enabled(bool enabled) { (void)enabled; }
ai_state_t ai_get_state(void) { return AI_STATE_DISABLED; }
bool ai_is_busy(void) { return false; }
bool ai_is_activated(void) { return false; }
const char *ai_get_activation_code(void) { return ""; }
const char *ai_get_error(void) { return ""; }
const char *ai_get_device_id(void) { return ""; }
const char *ai_get_ota_url(void) { return ""; }
bool ai_set_ota_url(const char *url) { (void)url; return false; }
bool ai_set_direct_ws(const char *url, const char *token) { (void)url; (void)token; return false; }
void ai_clear_activation(void) { }
void ai_request_listen(void) { }
void ai_stop_session(void) { }
void ai_abort_speaking(void) { }
void ai_retry(void) { }
void ai_set_muted(bool muted) { (void)muted; }
bool ai_is_muted(void) { return false; }
bool ai_wake_word_enabled(void) { return false; }
void ai_set_wake_word_enabled(bool enabled) { (void)enabled; }
bool ai_audio_pause_for_exclusive(void) { return false; }
void ai_audio_resume_after_exclusive(void) { }
void ai_set_external_audio_active(bool active) { (void)active; }

#endif
