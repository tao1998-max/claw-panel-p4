/*
 * Doubao (Volcengine) bidirectional streaming TTS for ESP-Claw.
 *
 * The HTTP API is intentionally tiny: credentials are stored in NVS and the
 * audio is played through a callback registered by main (the ES8311 path lives
 * there). `claw.speak(text)` and POST /api/tts/speak both use it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLAW_TTS_APP_ID_MAX     64
#define CLAW_TTS_TOKEN_MAX      192
#define CLAW_TTS_API_KEY_MAX    192
#define CLAW_TTS_RESOURCE_MAX   64
#define CLAW_TTS_SPEAKER_MAX    64

typedef struct {
    char app_id[CLAW_TTS_APP_ID_MAX];
    char access_token[CLAW_TTS_TOKEN_MAX];
    char api_key[CLAW_TTS_API_KEY_MAX];
    char resource_id[CLAW_TTS_RESOURCE_MAX];
    char speaker[CLAW_TTS_SPEAKER_MAX];
    bool auto_speak;
} claw_tts_config_t;

/* 16 kHz mono s16le chunks. Return false when the audio path is busy. */
typedef bool (*claw_tts_play_pcm_fn)(const int16_t *pcm, size_t samples,
                                     uint32_t timeout_ms);

/* Create the TTS locks and load persisted credentials (call after NVS init). */
void claw_tts_init(void);

/* Load/save the Doubao credentials from NVS. */
esp_err_t claw_tts_get_config(claw_tts_config_t *out);
esp_err_t claw_tts_set_config(const claw_tts_config_t *cfg);

bool claw_tts_is_configured(void);
bool claw_tts_auto_speak_enabled(void);

/* Registered by main after the ES8311 path is available. */
void claw_tts_set_playback(claw_tts_play_pcm_fn fn);

/* Synthesize and play text in one streaming session. Blocks for up to
 * timeout_ms (0 = 120 s). */
esp_err_t claw_tts_speak(const char *text, uint32_t timeout_ms);

/* Queue text for the auto-speak worker; returns immediately. */
esp_err_t claw_tts_speak_async(const char *text);

/* One WebSocket/session for a whole LLM round. Feed sentence chunks as they
 * arrive, then end() once; the worker sends FinishSession and plays remaining
 * audio. begin() returns once the worker owns the TTS lock. */
esp_err_t claw_tts_stream_begin(void);
esp_err_t claw_tts_stream_feed(const char *text);
esp_err_t claw_tts_stream_end(void);

#ifdef __cplusplus
}
#endif
