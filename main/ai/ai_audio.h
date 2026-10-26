#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 16 kHz ES8311 双工音频；AI 开启时替代 BSP 的 44.1 kHz 音频路径。 */
esp_err_t ai_audio_init(void);
void ai_audio_set_volume(int percent);
int ai_audio_get_volume(void);
void ai_audio_set_mute(bool muted);
bool ai_audio_is_muted(void);
void ai_audio_beep(void);
void ai_audio_chime(void);

#ifdef __cplusplus
}
#endif
