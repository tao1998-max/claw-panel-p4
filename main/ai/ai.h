#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AI_STATE_DISABLED = 0,
    AI_STATE_STARTING,
    AI_STATE_ACTIVATING,
    AI_STATE_IDLE,
    AI_STATE_CONNECTING,
    AI_STATE_LISTENING,
    AI_STATE_SPEAKING,
    AI_STATE_ERROR,
} ai_state_t;

typedef enum {
    AI_ROLE_SYSTEM = 0,
    AI_ROLE_USER,
    AI_ROLE_ASSISTANT,
} ai_role_t;

/* 由 app.c 在 settings_init 之后调用：挂载共享 FAT、读字体/模型。 */
esp_err_t ai_early_init(void);

/* 启动 AI 任务（非阻塞）。AI 编译关闭或运行时禁用时返回 ESP_ERR_NOT_SUPPORTED。 */
esp_err_t ai_start(void);
void ai_stop(void);

bool ai_available(void);
bool ai_enabled(void);
void ai_set_enabled(bool enabled);
ai_state_t ai_get_state(void);
bool ai_is_busy(void);
bool ai_is_activated(void);

const char *ai_get_activation_code(void);
const char *ai_get_error(void);
const char *ai_get_device_id(void);

const char *ai_get_ota_url(void);
bool ai_set_ota_url(const char *url);
bool ai_set_direct_ws(const char *url, const char *token);
void ai_clear_activation(void);

/* UI 操作 */
void ai_request_listen(void);
void ai_stop_session(void);
void ai_abort_speaking(void);
void ai_retry(void);

void ai_set_muted(bool muted);
bool ai_is_muted(void);
bool ai_wake_word_enabled(void);
void ai_set_wake_word_enabled(bool enabled);

/* Exclusive local playback: pause the assistant AudioService (and wait for its
 * tasks to exit) so the caller can reconfigure the shared ES8311/I2S. Resume
 * restarts the service and restores wake-word/voice-processing state. */
bool ai_audio_pause_for_exclusive(void);
void ai_audio_resume_after_exclusive(void);

/* Stop the assistant's microphone processing while another component plays
 * audio through the shared codec (Claw TTS), so wake word / ASR cannot hear
 * that audio. Assistant playback is unaffected. No-op when the assistant is
 * not running or is in a session. */
void ai_set_external_audio_active(bool active);

#ifdef __cplusplus
}
#endif
