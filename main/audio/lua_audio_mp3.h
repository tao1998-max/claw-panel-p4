/*
 * Local MP3 playback for the Lua audio module.
 *
 * The decoder runs on its own task because decoding a song blocks: the Lua
 * call must return immediately so the UI keeps running. PCM is fed to the
 * shared XiaoZhi AudioService at 16 kHz mono, which is the only format its
 * ES8311/I2S output exposes.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LUA_AUDIO_MP3_IDLE = 0,
    LUA_AUDIO_MP3_BUFFERING,
    LUA_AUDIO_MP3_PLAYING,
    LUA_AUDIO_MP3_FINISHED,
    LUA_AUDIO_MP3_STOPPED,
    LUA_AUDIO_MP3_ERROR,
} lua_audio_mp3_state_t;

/** True when the string is an http:// or https:// URL. */
bool lua_audio_mp3_is_url(const char *name);

/** Create the module's locks; safe to call more than once. */
esp_err_t lua_audio_mp3_init(void);

/**
 * Start decoding a local MP3 file on the audio task.
 *
 * Stops a previous MP3 playback first. When @p wait_ms is non-zero, waits for
 * the song to finish or the timeout. Returns false when the file/decoder is
 * unavailable; lua_audio_mp3_error() then carries the reason.
 */
bool lua_audio_mp3_play(const char *path, uint32_t wait_ms, const char *owner_job);

/** Stop the current MP3 playback and wait briefly for the task to exit. */
void lua_audio_mp3_stop(void);

/** Stop only if the current MP3 playback belongs to @p job_id (App exit). */
void lua_audio_mp3_cleanup_job(const char *job_id);

/** True while the MP3 task is running. */
bool lua_audio_mp3_active(void);

/** Last state reported by the task. */
lua_audio_mp3_state_t lua_audio_mp3_state(void);

/** Last error string (empty when none). */
const char *lua_audio_mp3_error(void);

/** Source format of the last decoded frame (0 until known). */
uint32_t lua_audio_mp3_source_rate(void);
uint8_t lua_audio_mp3_source_channels(void);

/** RMS level (0..1) of the most recent decoded frame, for visualizers. */
float lua_audio_mp3_level(void);

#ifdef __cplusplus
}
#endif
