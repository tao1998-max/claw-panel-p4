/*
 * Product Lua module: audio output, short OGG effects and native-rate MP3.
 *
 * The module sits on top of XiaoZhi's AudioService (ES8311 + I2S), so it never
 * opens a second codec. PCM is fed through the existing playback queue;
 * OGG effects use the same decoder as the assistant prompts. MP3 takes the
 * codec over exclusively (service paused, native rate) because the shared
 * service is fixed at 16 kHz mono.
 *
 * App-friendly form: audio.play("deal") resolves
 * <app-root>/assets/sfx/deal.ogg, where <app-root> is derived from the
 * currently running script (everything before its /scripts/ directory).
 */
#include "lua_module_audio.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ai_audio.h"
#include "cap_lua.h"
#include "dsps_fft2r.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lauxlib.h"
#include "lua.h"
#include "lua_audio_mp3.h"

#define AUDIO_PATH_MAX 256
#define AUDIO_OUTPUT_MT "audio.output"
#define AUDIO_INPUT_MT "audio.input"
#define AUDIO_RECORDER_MT "audio.recorder"
#define AUDIO_ANALYZER_MT "audio.analyzer"
#define AUDIO_PLAYER_MT "audio.player"

#define AUDIO_TONE_SAMPLE_RATE 16000
#define AUDIO_TONE_MAX_MS 5000
#define AUDIO_WRITE_TIMEOUT_MS 2000
#define AUDIO_INPUT_MAX_SAMPLES (16000 * 2)
#define AUDIO_READ_MAX_SAMPLES (16000 * 2)

typedef struct {
    bool open;
} audio_output_ud_t;

typedef struct {
    bool open;
    int volume;
} audio_input_ud_t;

typedef struct {
    bool open;
    int input_ref;
} audio_recorder_ud_t;

typedef struct {
    bool open;
    int input_ref;
} audio_analyzer_ud_t;

typedef struct {
    bool open;
    int output_ref;
    bool playing;
    bool stopped;
    bool mp3;
} audio_player_ud_t;

static bool s_output_open;
static bool s_input_open;
static bool s_recording;
static bool s_fft_initialized;

static bool audio_path_is_allowed(const char *path)
{
    if (path == NULL || path[0] != '/' || strstr(path, "..") != NULL) {
        return false;
    }

    static const char *const roots[] = {
        "/fatfs/claw/data/",
        "/fatfs/claw/system/",
        "/fatfs/ai/",
        "/sdcard/",
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); ++i) {
        if (strncmp(path, roots[i], strlen(roots[i])) == 0) {
            return true;
        }
    }
    return false;
}

static bool has_suffix_ci(const char *name, const char *suffix)
{
    size_t len = strlen(name);
    size_t suffix_len = strlen(suffix);
    return len >= suffix_len && strcasecmp(name + len - suffix_len, suffix) == 0;
}

static bool copy_app_root(const char *script_path, char *root, size_t root_size)
{
    const char *scripts = NULL;
    for (const char *p = script_path; (p = strstr(p, "/scripts/")) != NULL; p += 9) {
        scripts = p;
    }

    size_t len;
    if (scripts != NULL) {
        len = (size_t)(scripts - script_path);
    } else {
        const char *slash = strrchr(script_path, '/');
        if (slash == NULL) {
            return false;
        }
        len = (size_t)(slash - script_path);
    }
    if (len == 0 || len >= root_size) {
        return false;
    }
    memcpy(root, script_path, len);
    root[len] = '\0';
    return true;
}

static bool join_media_path(const char *root, const char *name, const char *subdir,
                            const char *suffix, char *out, size_t out_size)
{
    const char *ext = has_suffix_ci(name, suffix) ? "" : suffix;
    int written = snprintf(out, out_size, "%s/%s%s%s", root,
                           subdir != NULL ? subdir : "", name, ext);
    return written > 0 && (size_t)written < out_size;
}

static bool file_exists(const char *path)
{
    struct stat st = {0};
    return stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
}

static bool resolve_media_path(lua_State *L, const char *name, const char *suffix,
                               char *out, size_t out_size)
{
    if (name == NULL || name[0] == '\0' || strstr(name, "..") != NULL) {
        return false;
    }

    if (name[0] == '/') {
        if (!audio_path_is_allowed(name)) {
            return false;
        }
        strlcpy(out, name, out_size);
        return true;
    }

    const char *script_path = cap_lua_runtime_script_path(L);
    if (script_path == NULL) {
        return false;
    }

    char root[AUDIO_PATH_MAX];
    if (!copy_app_root(script_path, root, sizeof(root))) {
        return false;
    }

    char first[AUDIO_PATH_MAX];
    bool bare = strchr(name, '/') == NULL;
    if (bare) {
        /* audio.play("deal") -> <app>/assets/sfx/deal.ogg */
        if (!join_media_path(root, name, "assets/sfx/", suffix, first, sizeof(first))) {
            return false;
        }
    } else {
        /* audio.play("assets/sfx/deal.ogg") -> <app>/assets/sfx/deal.ogg */
        if (!join_media_path(root, name, "", suffix, first, sizeof(first))) {
            return false;
        }
    }

    if (file_exists(first)) {
        strlcpy(out, first, out_size);
        return audio_path_is_allowed(out);
    }

    /* Convenience: audio.play("sfx/deal") also finds <app>/assets/sfx/deal.ogg. */
    if (!bare) {
        char alt[AUDIO_PATH_MAX];
        if (join_media_path(root, name, "assets/", suffix, alt, sizeof(alt)) &&
            file_exists(alt)) {
            strlcpy(out, alt, out_size);
            return audio_path_is_allowed(out);
        }
    }

    /* Let the player report the missing file; keep the resolved path. */
    strlcpy(out, first, out_size);
    return audio_path_is_allowed(out);
}

static bool resolve_audio_path(lua_State *L, const char *name,
                               char *out, size_t out_size)
{
    return resolve_media_path(L, name, ".ogg", out, out_size);
}

/* Detect the container from the suffix so .mp3 goes to the MP3 task and
 * everything else keeps the existing OGG/Opus service path. */
static bool resolve_play_path(lua_State *L, const char *name,
                              char *out, size_t out_size, bool *is_mp3)
{
    *is_mp3 = has_suffix_ci(name, ".mp3");
    return resolve_media_path(L, name, *is_mp3 ? ".mp3" : ".ogg", out, out_size);
}

static audio_output_ud_t *check_output(lua_State *L, int index)
{
    audio_output_ud_t *ud = (audio_output_ud_t *)luaL_checkudata(L, index, AUDIO_OUTPUT_MT);
    if (!ud->open) {
        luaL_error(L, "audio output is closed");
    }
    return ud;
}

static int lua_audio_output_close(lua_State *L)
{
    audio_output_ud_t *ud = (audio_output_ud_t *)luaL_checkudata(L, 1, AUDIO_OUTPUT_MT);
    if (ud->open && s_output_open) {
        ud->open = false;
        s_output_open = false;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_audio_output_gc(lua_State *L)
{
    audio_output_ud_t *ud = (audio_output_ud_t *)luaL_checkudata(L, 1, AUDIO_OUTPUT_MT);
    if (ud->open && s_output_open) {
        ud->open = false;
        s_output_open = false;
    }
    return 0;
}

static int lua_audio_output_info(lua_State *L)
{
    (void)check_output(L, 1);
    int sample_rate = 0, channels = 0, bits = 0;
    ai_audio_output_format(&sample_rate, &channels, &bits);

    lua_newtable(L);
    lua_pushinteger(L, sample_rate);
    lua_setfield(L, -2, "sample_rate");
    lua_pushinteger(L, channels);
    lua_setfield(L, -2, "channels");
    lua_pushinteger(L, bits);
    lua_setfield(L, -2, "bits");
    return 1;
}

static int lua_audio_output_set_volume(lua_State *L)
{
    (void)check_output(L, 1);
    int volume = (int)luaL_checkinteger(L, 2);
    luaL_argcheck(L, volume >= 0 && volume <= 100, 2, "volume must be 0..100");
    ai_audio_set_volume(volume);
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_audio_output_get_volume(lua_State *L)
{
    (void)check_output(L, 1);
    lua_pushinteger(L, ai_audio_get_volume());
    return 1;
}

static int lua_audio_output_set_mute(lua_State *L)
{
    (void)check_output(L, 1);
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    /* The shared output has no independent mute; keep the upstream no-op
     * contract instead of muting the assistant microphone path. */
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_audio_output_write(lua_State *L)
{
    (void)check_output(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    if (len == 0 || (len & 1) != 0) {
        return luaL_error(L, "PCM write needs a non-empty 16-bit aligned string");
    }
    if (!ai_audio_write_pcm((const int16_t *)data, len / 2,
                            AUDIO_WRITE_TIMEOUT_MS)) {
        lua_pushnil(L);
        lua_pushstring(L, "audio output busy or assistant is active");
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)len);
    return 1;
}

static int lua_audio_output_play_tone(lua_State *L)
{
    (void)check_output(L, 1);
    int freq = (int)luaL_checkinteger(L, 2);
    int duration_ms = (int)luaL_checkinteger(L, 3);
    luaL_argcheck(L, freq > 0 && freq < AUDIO_TONE_SAMPLE_RATE / 2, 2,
                  "frequency must be below 8000 Hz");
    luaL_argcheck(L, duration_ms > 0 && duration_ms <= AUDIO_TONE_MAX_MS, 3,
                  "duration must be 1..5000 ms");

    size_t samples = (size_t)AUDIO_TONE_SAMPLE_RATE * (size_t)duration_ms / 1000;
    int16_t *pcm = (int16_t *)heap_caps_malloc(samples * sizeof(int16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pcm == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "out of memory");
        return 2;
    }

    const int fade = AUDIO_TONE_SAMPLE_RATE / 200; /* 5 ms */
    for (size_t i = 0; i < samples; ++i) {
        float envelope = 1.0f;
        if ((int)i < fade) envelope = (float)i / fade;
        else if ((int)(samples - i) < fade) envelope = (float)(samples - i) / fade;
        float value = sinf(2.0f * (float)M_PI * (float)freq * (float)i /
                           AUDIO_TONE_SAMPLE_RATE);
        pcm[i] = (int16_t)(8000.0f * envelope * value);
    }

    bool ok = ai_audio_write_pcm(pcm, samples, AUDIO_WRITE_TIMEOUT_MS);
    heap_caps_free(pcm);
    if (!ok) {
        lua_pushnil(L);
        lua_pushstring(L, "audio output busy or assistant is active");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static const luaL_Reg audio_output_methods[] = {
    {"close", lua_audio_output_close},
    {"info", lua_audio_output_info},
    {"set_volume", lua_audio_output_set_volume},
    {"get_volume", lua_audio_output_get_volume},
    {"set_mute", lua_audio_output_set_mute},
    {"write", lua_audio_output_write},
    {"play_tone", lua_audio_output_play_tone},
    {NULL, NULL},
};

static int lua_audio_open_output(lua_State *L)
{
    if (s_output_open) {
        lua_pushnil(L);
        lua_pushstring(L, "audio output already open");
        return 2;
    }

    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "volume");
        if (lua_isinteger(L, -1)) {
            int volume = (int)lua_tointeger(L, -1);
            if (volume >= 0 && volume <= 100) {
                ai_audio_set_volume(volume);
            }
        }
        lua_pop(L, 1);
    }

    audio_output_ud_t *ud = (audio_output_ud_t *)lua_newuserdata(
        L, sizeof(audio_output_ud_t));
    ud->open = true;
    s_output_open = true;
    luaL_getmetatable(L, AUDIO_OUTPUT_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static audio_input_ud_t *check_input(lua_State *L, int index)
{
    audio_input_ud_t *ud = (audio_input_ud_t *)luaL_checkudata(L, index, AUDIO_INPUT_MT);
    if (!ud->open) {
        luaL_error(L, "audio input is closed");
    }
    return ud;
}

static int lua_audio_input_close(lua_State *L)
{
    audio_input_ud_t *ud = (audio_input_ud_t *)luaL_checkudata(L, 1, AUDIO_INPUT_MT);
    if (ud->open) {
        ud->open = false;
        s_input_open = false;
        ai_audio_input_close();
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_audio_input_gc(lua_State *L)
{
    audio_input_ud_t *ud = (audio_input_ud_t *)luaL_checkudata(L, 1, AUDIO_INPUT_MT);
    if (ud->open) {
        ud->open = false;
        s_input_open = false;
        ai_audio_input_close();
    }
    return 0;
}

static int lua_audio_input_info(lua_State *L)
{
    (void)check_input(L, 1);
    lua_newtable(L);
    lua_pushinteger(L, 16000);
    lua_setfield(L, -2, "sample_rate");
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "channels");
    lua_pushinteger(L, 16);
    lua_setfield(L, -2, "bits");
    return 1;
}

static int lua_audio_input_set_volume(lua_State *L)
{
    audio_input_ud_t *ud = check_input(L, 1);
    int volume = (int)luaL_checkinteger(L, 2);
    luaL_argcheck(L, volume >= 0 && volume <= 100, 2, "volume must be 0..100");
    /* The ES8311 input gain is shared with XiaoZhi; keep this as a shadow
     * value so the API mirrors upstream without changing AFE behavior. */
    ud->volume = volume;
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_audio_input_get_volume(lua_State *L)
{
    audio_input_ud_t *ud = check_input(L, 1);
    lua_pushinteger(L, ud->volume);
    return 1;
}

static int lua_audio_input_read(lua_State *L)
{
    (void)check_input(L, 1);

    lua_Integer bytes = 0;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "bytes");
        bytes = luaL_checkinteger(L, -1);
        lua_pop(L, 1);
    } else {
        bytes = luaL_checkinteger(L, 2);
    }

    if ((bytes & 1) != 0) {
        return luaL_error(L, "input:read expects an even byte count");
    }
    size_t samples = (size_t)bytes / 2;
    luaL_argcheck(L, samples > 0 && samples <= AUDIO_READ_MAX_SAMPLES, 2,
                  "read size must be 2..64000 bytes");

    int16_t *pcm = (int16_t *)heap_caps_malloc(samples * sizeof(int16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pcm == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "out of memory");
        return 2;
    }

    size_t got = ai_audio_read_pcm(pcm, samples, 2000);
    if (got == 0) {
        heap_caps_free(pcm);
        lua_pushnil(L);
        lua_pushstring(L, "audio input timeout");
        return 2;
    }
    lua_pushlstring(L, (const char *)pcm, got * sizeof(int16_t));
    heap_caps_free(pcm);
    return 1;
}

static const luaL_Reg audio_input_methods[] = {
    {"close", lua_audio_input_close},
    {"info", lua_audio_input_info},
    {"set_volume", lua_audio_input_set_volume},
    {"get_volume", lua_audio_input_get_volume},
    {"read", lua_audio_input_read},
    {NULL, NULL},
};

static int lua_audio_open_input(lua_State *L)
{
    if (s_input_open) {
        lua_pushnil(L);
        lua_pushstring(L, "audio input already open");
        return 2;
    }

    int sample_rate = 16000;
    int channels = 1;
    int bits = 16;
    int volume = 100;
    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "sample_rate");
        if (lua_isinteger(L, -1)) sample_rate = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 1, "channels");
        if (lua_isinteger(L, -1)) channels = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 1, "bits");
        if (lua_isinteger(L, -1)) bits = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 1, "volume");
        if (lua_isinteger(L, -1)) volume = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }

    if (sample_rate != 16000 || channels != 1 || bits != 16) {
        lua_pushnil(L);
        lua_pushstring(L, "only 16000 Hz mono 16-bit input is supported");
        return 2;
    }
    if (volume < 0 || volume > 100) {
        lua_pushnil(L);
        lua_pushstring(L, "volume must be 0..100");
        return 2;
    }

    if (!ai_audio_input_open(AUDIO_INPUT_MAX_SAMPLES)) {
        lua_pushnil(L);
        lua_pushstring(L, "audio input unavailable (assistant disabled or muted)");
        return 2;
    }

    audio_input_ud_t *ud = (audio_input_ud_t *)lua_newuserdata(
        L, sizeof(audio_input_ud_t));
    ud->open = true;
    ud->volume = volume;
    s_input_open = true;
    luaL_getmetatable(L, AUDIO_INPUT_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static bool audio_recording_path_is_allowed(const char *path)
{
    if (path == NULL || path[0] != '/' || strstr(path, "..") != NULL) {
        return false;
    }
    return strncmp(path, "/fatfs/claw/data/", 17) == 0 ||
           strncmp(path, "/sdcard/", 8) == 0;
}

static bool ensure_parent_dir(const char *path)
{
    char parent[AUDIO_PATH_MAX];
    strlcpy(parent, path, sizeof(parent));
    char *slash = strrchr(parent, '/');
    if (slash == NULL || slash == parent) {
        return false;
    }
    *slash = '\0';

    struct stat st = {0};
    if (stat(parent, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return mkdir(parent, 0775) == 0 || errno == EEXIST;
}

static bool resolve_recording_path(lua_State *L, const char *name,
                                   char *out, size_t out_size)
{
    if (name == NULL || name[0] == '\0' || strstr(name, "..") != NULL) {
        return false;
    }

    if (name[0] == '/') {
        if (!audio_recording_path_is_allowed(name)) {
            return false;
        }
        strlcpy(out, name, out_size);
    } else {
        const char *script_path = cap_lua_runtime_script_path(L);
        if (script_path == NULL) {
            return false;
        }
        char root[AUDIO_PATH_MAX];
        if (!copy_app_root(script_path, root, sizeof(root))) {
            return false;
        }
        if (strchr(name, '/') != NULL) {
            if (snprintf(out, out_size, "%s/%s", root, name) >= (int)out_size) {
                return false;
            }
        } else {
            if (snprintf(out, out_size, "%s/recordings/%s", root, name) >= (int)out_size) {
                return false;
            }
        }
    }

    size_t len = strlen(out);
    if (len < 4 || strcmp(out + len - 4, ".wav") != 0) {
        if (strlcat(out, ".wav", out_size) >= out_size) {
            return false;
        }
    }

    return audio_recording_path_is_allowed(out) && ensure_parent_dir(out);
}

static void put_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xff);
    p[1] = (uint8_t)((value >> 8) & 0xff);
}

static void put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value & 0xff);
    p[1] = (uint8_t)((value >> 8) & 0xff);
    p[2] = (uint8_t)((value >> 16) & 0xff);
    p[3] = (uint8_t)((value >> 24) & 0xff);
}

static bool write_wav_header(FILE *file, uint32_t data_bytes)
{
    uint8_t header[44] = {0};
    memcpy(header, "RIFF", 4);
    put_le32(header + 4, 36 + data_bytes);
    memcpy(header + 8, "WAVEfmt ", 8);
    put_le32(header + 16, 16);
    put_le16(header + 20, 1);
    put_le16(header + 22, 1);
    put_le32(header + 24, 16000);
    put_le32(header + 28, 16000 * 2);
    put_le16(header + 32, 2);
    put_le16(header + 34, 16);
    memcpy(header + 36, "data", 4);
    put_le32(header + 40, data_bytes);
    return fwrite(header, 1, sizeof(header), file) == sizeof(header);
}

static audio_recorder_ud_t *check_recorder(lua_State *L, int index)
{
    audio_recorder_ud_t *ud = (audio_recorder_ud_t *)luaL_checkudata(L, index, AUDIO_RECORDER_MT);
    if (!ud->open) {
        luaL_error(L, "audio recorder is closed");
    }
    return ud;
}

static int lua_audio_recorder_close(lua_State *L)
{
    audio_recorder_ud_t *ud = (audio_recorder_ud_t *)luaL_checkudata(L, 1, AUDIO_RECORDER_MT);
    if (ud->open) {
        ud->open = false;
        if (ud->input_ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, ud->input_ref);
            ud->input_ref = LUA_NOREF;
        }
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_audio_recorder_gc(lua_State *L)
{
    audio_recorder_ud_t *ud = (audio_recorder_ud_t *)luaL_checkudata(L, 1, AUDIO_RECORDER_MT);
    if (ud->input_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ud->input_ref);
        ud->input_ref = LUA_NOREF;
    }
    ud->open = false;
    return 0;
}

static int lua_audio_recorder_record(lua_State *L)
{
    (void)check_recorder(L, 1);
    const char *name = luaL_checkstring(L, 2);
    int duration_ms = 5000;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "duration_ms");
        if (lua_isinteger(L, -1)) {
            duration_ms = (int)lua_tointeger(L, -1);
        }
        lua_pop(L, 1);
    }
    luaL_argcheck(L, duration_ms > 0 && duration_ms <= 60000, 3,
                  "duration_ms must be 1..60000");

    if (s_recording) {
        lua_pushnil(L);
        lua_pushstring(L, "recorder is busy");
        return 2;
    }

    char path[AUDIO_PATH_MAX];
    if (!resolve_recording_path(L, name, path, sizeof(path))) {
        lua_pushnil(L);
        lua_pushstring(L, "recording path must be under /fatfs/claw/data or /sdcard");
        return 2;
    }

    size_t target_samples = (size_t)16000 * (size_t)duration_ms / 1000;

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "failed to create recording file");
        return 2;
    }
    if (!write_wav_header(file, 0)) {
        fclose(file);
        unlink(path);
        lua_pushnil(L);
        lua_pushstring(L, "failed to write WAV header");
        return 2;
    }

    const size_t chunk_samples = 1600;
    int16_t *pcm = (int16_t *)heap_caps_malloc(chunk_samples * sizeof(int16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pcm == NULL) {
        fclose(file);
        unlink(path);
        lua_pushnil(L);
        lua_pushstring(L, "out of memory");
        return 2;
    }

    size_t total = 0;
    bool ok = true;
    s_recording = true;
    while (total < target_samples) {
        size_t want = target_samples - total;
        if (want > chunk_samples) want = chunk_samples;
        size_t got = ai_audio_read_pcm(pcm, want, 1500);
        if (got == 0) {
            ok = false;
            break;
        }
        if (fwrite(pcm, sizeof(int16_t), got, file) != got) {
            ok = false;
            break;
        }
        total += got;
    }
    s_recording = false;
    heap_caps_free(pcm);

    if (ok && total > 0) {
        if (fseek(file, 0, SEEK_SET) != 0 ||
            !write_wav_header(file, (uint32_t)(total * sizeof(int16_t)))) {
            ok = false;
        }
    }
    if (fclose(file) != 0) {
        ok = false;
    }
    if (!ok || total == 0) {
        unlink(path);
        lua_pushnil(L);
        lua_pushstring(L, "recording failed (input timeout or storage error)");
        return 2;
    }

    lua_newtable(L);
    lua_pushstring(L, path);
    lua_setfield(L, -2, "path");
    lua_pushinteger(L, (lua_Integer)duration_ms);
    lua_setfield(L, -2, "duration_ms");
    lua_pushinteger(L, (lua_Integer)(total * sizeof(int16_t)));
    lua_setfield(L, -2, "bytes");
    lua_pushstring(L, "wav");
    lua_setfield(L, -2, "format");
    return 1;
}

static const luaL_Reg audio_recorder_methods[] = {
    {"record", lua_audio_recorder_record},
    {"close", lua_audio_recorder_close},
    {NULL, NULL},
};

static int lua_audio_recorder_new(lua_State *L)
{
    if (!lua_istable(L, 1)) {
        return luaL_error(L, "audio.recorder expects { input = input }");
    }
    lua_getfield(L, 1, "input");
    luaL_checkudata(L, -1, AUDIO_INPUT_MT);
    int input_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    audio_recorder_ud_t *ud = (audio_recorder_ud_t *)lua_newuserdata(
        L, sizeof(audio_recorder_ud_t));
    ud->open = true;
    ud->input_ref = input_ref;
    luaL_getmetatable(L, AUDIO_RECORDER_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static audio_analyzer_ud_t *check_analyzer(lua_State *L, int index)
{
    audio_analyzer_ud_t *ud = (audio_analyzer_ud_t *)luaL_checkudata(L, index, AUDIO_ANALYZER_MT);
    if (!ud->open) {
        luaL_error(L, "audio analyzer is closed");
    }
    return ud;
}

static int lua_audio_analyzer_close(lua_State *L)
{
    audio_analyzer_ud_t *ud = (audio_analyzer_ud_t *)luaL_checkudata(L, 1, AUDIO_ANALYZER_MT);
    if (ud->open) {
        ud->open = false;
        if (ud->input_ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, ud->input_ref);
            ud->input_ref = LUA_NOREF;
        }
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_audio_analyzer_gc(lua_State *L)
{
    audio_analyzer_ud_t *ud = (audio_analyzer_ud_t *)luaL_checkudata(L, 1, AUDIO_ANALYZER_MT);
    if (ud->input_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ud->input_ref);
        ud->input_ref = LUA_NOREF;
    }
    ud->open = false;
    return 0;
}

static int lua_audio_analyzer_read_level(lua_State *L)
{
    (void)check_analyzer(L, 1);
    int duration_ms = 100;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "duration_ms");
        if (lua_isinteger(L, -1)) duration_ms = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
    } else if (lua_isinteger(L, 2)) {
        duration_ms = (int)lua_tointeger(L, 2);
    }
    luaL_argcheck(L, duration_ms > 0 && duration_ms <= 1000, 2,
                  "duration_ms must be 1..1000");

    size_t samples = (size_t)16000 * (size_t)duration_ms / 1000;
    int16_t *pcm = (int16_t *)heap_caps_malloc(samples * sizeof(int16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pcm == NULL) {
        lua_pushnil(L);
        lua_pushstring(L, "out of memory");
        return 2;
    }
    size_t got = ai_audio_read_pcm(pcm, samples, 2000);
    if (got == 0) {
        heap_caps_free(pcm);
        lua_pushnil(L);
        lua_pushstring(L, "audio input timeout");
        return 2;
    }

    double sum = 0.0;
    int peak = 0;
    for (size_t i = 0; i < got; ++i) {
        int value = pcm[i];
        if (value < 0) value = -value;
        if (value > peak) peak = value;
        sum += (double)pcm[i] * (double)pcm[i];
    }
    double rms = sqrt(sum / (double)got) / 32768.0;
    heap_caps_free(pcm);

    lua_newtable(L);
    lua_pushnumber(L, rms);
    lua_setfield(L, -2, "rms");
    lua_pushinteger(L, (lua_Integer)peak);
    lua_setfield(L, -2, "peak");
    lua_pushinteger(L, duration_ms);
    lua_setfield(L, -2, "duration_ms");
    return 1;
}

static int lua_audio_analyzer_read_spectrum(lua_State *L)
{
    (void)check_analyzer(L, 1);
    int fft_size = 512;
    int bands = 16;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "fft_size");
        if (lua_isinteger(L, -1)) fft_size = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "bands");
        if (lua_isinteger(L, -1)) bands = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
    } else {
        if (lua_isinteger(L, 2)) fft_size = (int)lua_tointeger(L, 2);
        if (lua_isinteger(L, 3)) bands = (int)lua_tointeger(L, 3);
    }
    luaL_argcheck(L, fft_size >= 64 && fft_size <= 4096 &&
                  (fft_size & (fft_size - 1)) == 0, 2,
                  "fft_size must be a power of two between 64 and 4096");
    luaL_argcheck(L, bands >= 1 && bands <= fft_size / 2, 3,
                  "bands must be 1..fft_size/2");

    if (!s_fft_initialized) {
        if (dsps_fft2r_init_fc32(NULL, 4096) != ESP_OK) {
            lua_pushnil(L);
            lua_pushstring(L, "FFT initialization failed");
            return 2;
        }
        s_fft_initialized = true;
    }

    float *fft = (float *)heap_caps_calloc((size_t)fft_size * 2, sizeof(float),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *pcm = (int16_t *)heap_caps_malloc((size_t)fft_size * sizeof(int16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (fft == NULL || pcm == NULL) {
        heap_caps_free(fft);
        heap_caps_free(pcm);
        lua_pushnil(L);
        lua_pushstring(L, "out of memory");
        return 2;
    }

    size_t got = ai_audio_read_pcm(pcm, (size_t)fft_size, 2000);
    if (got == 0) {
        heap_caps_free(fft);
        heap_caps_free(pcm);
        lua_pushnil(L);
        lua_pushstring(L, "audio input timeout");
        return 2;
    }

    double sum = 0.0;
    for (int i = 0; i < fft_size; ++i) {
        int16_t sample = i < (int)got ? pcm[i] : 0;
        sum += (double)sample * (double)sample;
        double window = 0.5 - 0.5 * cos(2.0 * M_PI * i / (fft_size - 1));
        fft[2 * i] = (float)(sample * window);
        fft[2 * i + 1] = 0.0f;
    }

    dsps_fft2r_fc32(fft, fft_size);
    dsps_bit_rev_fc32(fft, fft_size);

    int peak_bin = 0;
    float peak_mag = 0.0f;
    for (int i = 1; i < fft_size / 2; ++i) {
        float re = fft[2 * i];
        float im = fft[2 * i + 1];
        float mag = sqrtf(re * re + im * im);
        if (mag > peak_mag) {
            peak_mag = mag;
            peak_bin = i;
        }
    }

    lua_newtable(L);
    lua_newtable(L);
    for (int b = 0; b < bands; ++b) {
        int start = 1 + (int)((int64_t)b * (fft_size / 2 - 1) / bands);
        int end = 1 + (int)((int64_t)(b + 1) * (fft_size / 2 - 1) / bands);
        if (end <= start) end = start + 1;
        if (end > fft_size / 2) end = fft_size / 2;
        float band_mag = 0.0f;
        for (int i = start; i < end; ++i) {
            float re = fft[2 * i];
            float im = fft[2 * i + 1];
            float mag = sqrtf(re * re + im * im);
            if (mag > band_mag) band_mag = mag;
        }
        float db = 20.0f * log10f(band_mag + 1e-6f);
        int level = (int)((db + 90.0f) / 70.0f * 255.0f);
        if (level < 0) level = 0;
        if (level > 255) level = 255;
        lua_pushinteger(L, level);
        lua_rawseti(L, -2, b + 1);
    }
    lua_setfield(L, -2, "bands");

    float peak_db = 20.0f * log10f(peak_mag + 1e-6f);
    lua_pushinteger(L, (lua_Integer)((int64_t)peak_bin * 16000 / fft_size));
    lua_setfield(L, -2, "peak_freq_hz");
    lua_pushnumber(L, peak_db);
    lua_setfield(L, -2, "peak_db");
    lua_pushnumber(L, sqrt(sum / (double)fft_size) / 32768.0);
    lua_setfield(L, -2, "rms");
    lua_pushinteger(L, fft_size);
    lua_setfield(L, -2, "fft_size");
    lua_pushinteger(L, bands);
    lua_setfield(L, -2, "band_count");
    lua_pushinteger(L, 16000);
    lua_setfield(L, -2, "sample_rate");

    heap_caps_free(fft);
    heap_caps_free(pcm);
    return 1;
}

static const luaL_Reg audio_analyzer_methods[] = {
    {"read_level", lua_audio_analyzer_read_level},
    {"read_spectrum", lua_audio_analyzer_read_spectrum},
    {"close", lua_audio_analyzer_close},
    {NULL, NULL},
};

static int lua_audio_analyzer_new(lua_State *L)
{
    if (!lua_istable(L, 1)) {
        return luaL_error(L, "audio.analyzer expects { input = input }");
    }
    lua_getfield(L, 1, "input");
    luaL_checkudata(L, -1, AUDIO_INPUT_MT);
    int input_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    audio_analyzer_ud_t *ud = (audio_analyzer_ud_t *)lua_newuserdata(
        L, sizeof(audio_analyzer_ud_t));
    ud->open = true;
    ud->input_ref = input_ref;
    luaL_getmetatable(L, AUDIO_ANALYZER_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static audio_player_ud_t *check_player(lua_State *L, int index)
{
    audio_player_ud_t *ud = (audio_player_ud_t *)luaL_checkudata(L, index, AUDIO_PLAYER_MT);
    if (!ud->open) {
        luaL_error(L, "audio player is closed");
    }
    return ud;
}

static int lua_audio_player_close(lua_State *L)
{
    audio_player_ud_t *ud = (audio_player_ud_t *)luaL_checkudata(L, 1, AUDIO_PLAYER_MT);
    if (ud->open) {
        if (ud->mp3) {
            lua_audio_mp3_stop();
            ud->mp3 = false;
        }
        ud->open = false;
        ud->playing = false;
        if (ud->output_ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, ud->output_ref);
            ud->output_ref = LUA_NOREF;
        }
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_audio_player_gc(lua_State *L)
{
    audio_player_ud_t *ud = (audio_player_ud_t *)luaL_checkudata(L, 1, AUDIO_PLAYER_MT);
    if (ud->mp3) {
        lua_audio_mp3_stop();
        ud->mp3 = false;
    }
    if (ud->output_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ud->output_ref);
        ud->output_ref = LUA_NOREF;
    }
    ud->open = false;
    ud->playing = false;
    return 0;
}

static int lua_audio_player_play(lua_State *L)
{
    audio_player_ud_t *ud = check_player(L, 1);
    const char *name = luaL_checkstring(L, 2);
    bool wait = false;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "wait");
        wait = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }

    if (lua_audio_mp3_is_url(name)) {
        lua_audio_mp3_stop();
        if (!lua_audio_mp3_play(name, wait ? 60000 : 0, cap_lua_runtime_job_id(L))) {
            lua_pushnil(L);
            lua_pushstring(L, lua_audio_mp3_error());
            return 2;
        }
        ud->playing = true;
        ud->stopped = false;
        ud->mp3 = true;
        lua_pushboolean(L, 1);
        return 1;
    }

    char path[AUDIO_PATH_MAX];
    bool is_mp3 = false;
    if (!resolve_play_path(L, name, path, sizeof(path), &is_mp3)) {
        lua_pushnil(L);
        lua_pushstring(L, "player path must be app-relative or an allowed absolute path");
        return 2;
    }

    if (is_mp3) {
        lua_audio_mp3_stop();
        if (!lua_audio_mp3_play(path, wait ? 60000 : 0, cap_lua_runtime_job_id(L))) {
            lua_pushnil(L);
            lua_pushstring(L, lua_audio_mp3_error());
            return 2;
        }
        ud->playing = true;
        ud->stopped = false;
        ud->mp3 = true;
        lua_pushboolean(L, 1);
        return 1;
    }

    ud->mp3 = false;
    if (ud->playing && !ai_audio_playback_idle()) {
        ai_audio_stop_playback();
    }
    if (!ai_audio_play_ogg_file(path)) {
        lua_pushnil(L);
        lua_pushstring(L, "player busy or file missing");
        return 2;
    }

    ud->playing = true;
    ud->stopped = false;
    if (wait) {
        const int64_t deadline = esp_timer_get_time() + 60000000LL;
        while (!ai_audio_playback_idle() && esp_timer_get_time() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        ud->playing = false;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_audio_player_stop(lua_State *L)
{
    audio_player_ud_t *ud = check_player(L, 1);
    if (ud->mp3) {
        lua_audio_mp3_stop();
        ud->mp3 = false;
    } else if (ud->playing) {
        ai_audio_stop_playback();
    }
    ud->playing = false;
    ud->stopped = true;
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_audio_player_poll(lua_State *L)
{
    audio_player_ud_t *ud = check_player(L, 1);
    const char *state = "idle";
    bool running = false;
    bool is_mp3 = ud->mp3;

    if (is_mp3) {
        switch (lua_audio_mp3_state()) {
        case LUA_AUDIO_MP3_BUFFERING:
            state = "buffering";
            running = true;
            break;
        case LUA_AUDIO_MP3_PLAYING:
            state = "playing";
            running = true;
            break;
        case LUA_AUDIO_MP3_FINISHED:
            state = "finished";
            ud->playing = false;
            break;
        case LUA_AUDIO_MP3_STOPPED:
            state = "stopped";
            ud->playing = false;
            break;
        case LUA_AUDIO_MP3_ERROR:
            state = "error";
            ud->playing = false;
            break;
        default:
            state = "idle";
            ud->playing = false;
            break;
        }
    } else {
        bool idle = ai_audio_playback_idle();
        if (ud->stopped) {
            state = "stopped";
        } else if (ud->playing) {
            state = idle ? "finished" : "playing";
            running = !idle;
        }
    }

    lua_newtable(L);
    lua_pushstring(L, state);
    lua_setfield(L, -2, "state");
    lua_pushboolean(L, running);
    lua_setfield(L, -2, "running");
    uint32_t output_rate = 16000;
    uint8_t output_channels = 1;
    if (is_mp3) {
        output_rate = lua_audio_mp3_source_rate();
        if (output_rate == 0) {
            output_rate = 16000;
        }
        output_channels = 2; /* exclusive MP3 output is always stereo */
    }
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)output_rate);
    lua_setfield(L, -2, "sample_rate");
    lua_pushinteger(L, output_channels);
    lua_setfield(L, -2, "channels");
    lua_pushinteger(L, 16);
    lua_setfield(L, -2, "bits");
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "bitrate");
    if (is_mp3) {
        lua_pushinteger(L, (lua_Integer)lua_audio_mp3_source_rate());
        lua_setfield(L, -2, "source_sample_rate");
        lua_pushinteger(L, (lua_Integer)lua_audio_mp3_source_channels());
        lua_setfield(L, -2, "source_channels");
    }
    lua_setfield(L, -2, "music_info");
    if (is_mp3) {
        lua_pushnumber(L, lua_audio_mp3_level());
        lua_setfield(L, -2, "level");
    }
    if (is_mp3 && !running && lua_audio_mp3_state() == LUA_AUDIO_MP3_ERROR) {
        lua_pushstring(L, lua_audio_mp3_error());
        lua_setfield(L, -2, "error");
    }
    return 1;
}

static int lua_audio_player_unsupported(lua_State *L)
{
    lua_pushnil(L);
    lua_pushstring(L, "unsupported by the shared AudioService player");
    return 2;
}

static const luaL_Reg audio_player_methods[] = {
    {"play", lua_audio_player_play},
    {"stop", lua_audio_player_stop},
    {"pause", lua_audio_player_unsupported},
    {"resume", lua_audio_player_unsupported},
    {"poll", lua_audio_player_poll},
    {"close", lua_audio_player_close},
    {NULL, NULL},
};

static int lua_audio_player_new(lua_State *L)
{
    if (!lua_istable(L, 1)) {
        return luaL_error(L, "audio.player expects { output = output }");
    }
    lua_getfield(L, 1, "output");
    luaL_checkudata(L, -1, AUDIO_OUTPUT_MT);
    int output_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    audio_player_ud_t *ud = (audio_player_ud_t *)lua_newuserdata(
        L, sizeof(audio_player_ud_t));
    ud->open = true;
    ud->output_ref = output_ref;
    ud->playing = false;
    ud->stopped = false;
    ud->mp3 = false;
    luaL_getmetatable(L, AUDIO_PLAYER_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static int lua_audio_play(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    if (lua_audio_mp3_is_url(name)) {
        lua_pushboolean(L, lua_audio_mp3_play(name, 0, cap_lua_runtime_job_id(L)));
        return 1;
    }

    char path[AUDIO_PATH_MAX];
    bool is_mp3 = false;

    if (!resolve_play_path(L, name, path, sizeof(path), &is_mp3)) {
        return luaL_error(L,
                          "audio.play: '%s' is outside the app/data roots or uses '..'",
                          name);
    }

    if (is_mp3) {
        lua_pushboolean(L, lua_audio_mp3_play(path, 0, cap_lua_runtime_job_id(L)));
        return 1;
    }
    lua_pushboolean(L, ai_audio_play_ogg_file(path));
    return 1;
}

static int lua_audio_is_available(lua_State *L)
{
    lua_pushboolean(L, ai_audio_available());
    return 1;
}

static const luaL_Reg audio_funcs[] = {
    {"open_output", lua_audio_open_output},
    {"open_input", lua_audio_open_input},
    {"player", lua_audio_player_new},
    {"recorder", lua_audio_recorder_new},
    {"analyzer", lua_audio_analyzer_new},
    {"play", lua_audio_play},
    {"is_available", lua_audio_is_available},
    {NULL, NULL},
};

void lua_audio_exit_cleanup(lua_State *L)
{
    lua_audio_mp3_cleanup_job(cap_lua_runtime_job_id(L));
}

int luaopen_audio(lua_State *L)
{
    luaL_newmetatable(L, AUDIO_OUTPUT_MT);
    lua_pushcfunction(L, lua_audio_output_gc);
    lua_setfield(L, -2, "__gc");
    lua_newtable(L);
    luaL_setfuncs(L, audio_output_methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    luaL_newmetatable(L, AUDIO_INPUT_MT);
    lua_pushcfunction(L, lua_audio_input_gc);
    lua_setfield(L, -2, "__gc");
    lua_newtable(L);
    luaL_setfuncs(L, audio_input_methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    luaL_newmetatable(L, AUDIO_RECORDER_MT);
    lua_pushcfunction(L, lua_audio_recorder_gc);
    lua_setfield(L, -2, "__gc");
    lua_newtable(L);
    luaL_setfuncs(L, audio_recorder_methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    luaL_newmetatable(L, AUDIO_ANALYZER_MT);
    lua_pushcfunction(L, lua_audio_analyzer_gc);
    lua_setfield(L, -2, "__gc");
    lua_newtable(L);
    luaL_setfuncs(L, audio_analyzer_methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    luaL_newmetatable(L, AUDIO_PLAYER_MT);
    lua_pushcfunction(L, lua_audio_player_gc);
    lua_setfield(L, -2, "__gc");
    lua_newtable(L);
    luaL_setfuncs(L, audio_player_methods, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    (void)lua_audio_mp3_init();
    luaL_newlib(L, audio_funcs);
    return 1;
}
