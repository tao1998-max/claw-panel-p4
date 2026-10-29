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

/* Short UI/game effects: decode an OGG/Opus file into the speaker queue.
 * Returns false when the XiaoZhi audio service is not running/available. */
bool ai_audio_available(void);
bool ai_audio_play_ogg_file(const char *path);

/* PCM output used by the Lua audio module. The format is fixed by the ES8311
 * path (see ai_audio_output_format); samples are 16-bit mono. */
void ai_audio_output_format(int *sample_rate, int *channels, int *bits);
bool ai_audio_write_pcm(const int16_t *pcm, size_t samples, uint32_t timeout_ms);

/* Raw microphone tap for Lua input/recorder/analyzer. Returns 16 kHz mono
 * 16-bit PCM copied from the same frames that feed WakeNet/AFE. */
bool ai_audio_input_open(size_t max_samples);
size_t ai_audio_read_pcm(int16_t *pcm, size_t samples, uint32_t timeout_ms);
void ai_audio_input_close(void);

/* Playback state for the Lua player object. */
bool ai_audio_playback_idle(void);
void ai_audio_stop_playback(void);

#ifdef __cplusplus
}
#endif
