/*
 * Local MP3 playback for the Lua audio module.
 *
 * MP3 is decoded at its native sample rate and written straight to the ES8311
 * through an exclusive output: the assistant's AudioService is paused first
 * (otherwise its 16 kHz duplex path owns the same I2S), then the codec is
 * reconfigured to the MP3 rate. On stop/EOF the codec is restored to 16 kHz
 * and the assistant restarts. This means wake word / voice is unavailable
 * while an MP3 is playing, which is the trade-off for native-rate audio.
 */
#include "lua_audio_mp3.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai_audio.h"
#include "cap_lua.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_types.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mp3_dec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#define MP3_READ_SIZE       4096
#define MP3_OUT_SIZE        8192
#define MP3_MAX_SAMPLES     4096
#define MP3_MIN_RATE        8000
#define MP3_MAX_RATE        48000
#define MP3_TASK_STACK      12288
#define MP3_TASK_PRIO       5
#define MP3_OUT_TASK_STACK  6144
#define MP3_OUT_TASK_PRIO   6
#define MP3_STOP_WAIT_MS    3000
#define MP3_STREAM_RETRIES  8
#define MP3_PCM_SECONDS     4
#define MP3_PREBUFFER_MS    1500

/* vStreamBufferDeleteWithCaps() must not be used on the buffer below. It
 * deletes the statically-created object through the queue API (vQueueDelete),
 * which reads a Queue_t ucStaticallyAllocated byte that does not exist in
 * StreamBuffer_t and can therefore free the control block; the wrapper then
 * frees the same block again (tlsf double free), which is what made track
 * switching reboot the device. Free the two blocks ourselves. */
static void mp3_stream_buffer_destroy(StreamBufferHandle_t handle)
{
    if (handle == NULL) {
        return;
    }
    uint8_t *storage = NULL;
    StaticStreamBuffer_t *control = NULL;
    if (xStreamBufferGetStaticBuffers(handle, &storage, &control) == pdTRUE) {
        vStreamBufferDelete(handle); /* static: scrubs, does not free */
        heap_caps_free(control);
        heap_caps_free(storage);
    } else {
        vStreamBufferDelete(handle);
    }
}

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_done;
static TaskHandle_t s_task;
static volatile bool s_stop;
static volatile bool s_is_stream;
static volatile lua_audio_mp3_state_t s_state = LUA_AUDIO_MP3_IDLE;
static volatile uint32_t s_src_rate;
static volatile uint8_t s_src_channels;
static volatile float s_level;
static char s_path[512];
static char s_owner_job[32];
static char s_error[96];
static bool s_decoder_registered;

static void mp3_set_error(const char *message)
{
    strlcpy(s_error, message ? message : "unknown error", sizeof(s_error));
}

bool lua_audio_mp3_is_url(const char *name)
{
    return name != NULL &&
           (strncmp(name, "http://", 7) == 0 || strncmp(name, "https://", 8) == 0);
}

/* The App that started playback may exit without closing its player; stop the
 * MP3 task as soon as its owning Lua job disappears. */
static bool mp3_owner_active(const char *job_id)
{
    if (job_id == NULL || job_id[0] == '\0') {
        return true;
    }
    cap_lua_job_snapshot_t jobs[8];
    size_t count = cap_lua_collect_active_jobs(jobs, 8);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(jobs[i].job_id, job_id) == 0) {
            return true;
        }
    }
    return false;
}

static esp_http_client_handle_t mp3_stream_open(void)
{
    esp_http_client_config_t cfg = {
        .url = s_path,
        .timeout_ms = 5000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .keep_alive_enable = false,
        .disable_auto_redirect = false,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "ESP32-P4-Radio/1.0",
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return NULL;
    }
    /* Do not request ICY metadata: the body stays pure MP3. */
    (void)esp_http_client_set_header(client, "Icy-MetaData", "0");
    if (esp_http_client_open(client, 0) != ESP_OK) {
        esp_http_client_cleanup(client);
        return NULL;
    }
    (void)esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 400) {
        ESP_LOGW("audio_mp3", "stream HTTP status %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return NULL;
    }
    /* Short read timeout so stop requests are observed quickly. */
    (void)esp_http_client_set_timeout_ms(client, 1000);
    return client;
}

static void mp3_stream_close(esp_http_client_handle_t *client)
{
    if (*client != NULL) {
        esp_http_client_close(*client);
        esp_http_client_cleanup(*client);
        *client = NULL;
    }
}

typedef struct {
    StreamBufferHandle_t pcm;
    SemaphoreHandle_t done;
    TaskHandle_t task;
    volatile bool stop;
    volatile bool producer_done;
    volatile bool failed;
    char error[96];
} mp3_out_ctx_t;

static void mp3_output_task(void *arg)
{
    mp3_out_ctx_t *out = (mp3_out_ctx_t *)arg;
    int16_t chunk[512 * 2];

    while (!out->stop) {
        size_t got = xStreamBufferReceive(out->pcm, chunk, sizeof(chunk), pdMS_TO_TICKS(50));
        if (got >= sizeof(int16_t) * 2) {
            int frames = (int)(got / (sizeof(int16_t) * 2));
            if (!ai_audio_exclusive_write(chunk, frames)) {
                if (!out->stop) {
                    strlcpy(out->error, "audio write failed", sizeof(out->error));
                    out->failed = true;
                }
                break;
            }
        } else if (out->producer_done) {
            break;
        }
    }
    xSemaphoreGive(out->done);
    vTaskDeleteWithCaps(NULL);
}

static bool mp3_start_output(mp3_out_ctx_t *out)
{
    if (out->task != NULL) {
        return true;
    }
    BaseType_t ok = xTaskCreateWithCaps(mp3_output_task, "mp3_out", MP3_OUT_TASK_STACK,
                                        out, MP3_OUT_TASK_PRIO, &out->task,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return ok == pdPASS;
}

static void mp3_task(void *arg)
{
    (void)arg;
    FILE *file = NULL;
    esp_http_client_handle_t http = NULL;
    esp_audio_simple_dec_handle_t decoder = NULL;
    uint8_t *input = NULL;
    uint8_t *output = NULL;
    int16_t *stereo = NULL;
    mp3_out_ctx_t out = {0};
    bool out_started = false;
    bool failed = false;
    bool exclusive = false;
    bool stream = s_is_stream;
    size_t decoded_frames = 0;
    size_t prebuffer_bytes = 0;
    uint32_t format_rate = 0;
    uint8_t format_channels = 0;
    int stream_failures = 0;
    int read_timeouts = 0;

    s_state = stream ? LUA_AUDIO_MP3_BUFFERING : LUA_AUDIO_MP3_PLAYING;
    s_error[0] = '\0';
    s_src_rate = 0;
    s_src_channels = 0;
    s_level = 0;

    if (stream) {
        http = mp3_stream_open();
        if (http == NULL) {
            mp3_set_error("stream unavailable");
            failed = true;
            goto cleanup;
        }
    } else {
        file = fopen(s_path, "rb");
        if (file == NULL) {
            mp3_set_error("file missing");
            failed = true;
            goto cleanup;
        }
    }
    if (!s_decoder_registered && esp_mp3_dec_register() == ESP_AUDIO_ERR_OK) {
        s_decoder_registered = true;
    }
    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .dec_cfg = NULL,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    if (esp_audio_simple_dec_open(&cfg, &decoder) != ESP_AUDIO_ERR_OK) {
        mp3_set_error("MP3 decoder unavailable");
        failed = true;
        goto cleanup;
    }

    input = heap_caps_malloc(MP3_READ_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    output = heap_caps_malloc(MP3_OUT_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    stereo = heap_caps_malloc(MP3_MAX_SAMPLES * 2 * sizeof(int16_t),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (input == NULL || output == NULL || stereo == NULL) {
        mp3_set_error("out of memory");
        failed = true;
        goto cleanup;
    }

    TickType_t last_owner_check = xTaskGetTickCount();
    while (!s_stop) {
        if ((xTaskGetTickCount() - last_owner_check) >= pdMS_TO_TICKS(250)) {
            last_owner_check = xTaskGetTickCount();
            if (!mp3_owner_active(s_owner_job)) {
                s_stop = true;
                break;
            }
        }

        int read_bytes;
        if (stream) {
            if (http == NULL) {
                if (++stream_failures > MP3_STREAM_RETRIES) {
                    mp3_set_error("stream unavailable");
                    failed = true;
                    goto cleanup;
                }
                s_state = LUA_AUDIO_MP3_BUFFERING;
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (s_stop) {
                    break;
                }
                http = mp3_stream_open();
                if (http != NULL) {
                    stream_failures = 0;
                    read_timeouts = 0;
                    (void)esp_audio_simple_dec_reset(decoder);
                }
                continue;
            }
            read_bytes = esp_http_client_read(http, (char *)input, MP3_READ_SIZE);
            if (read_bytes <= 0) {
                if (esp_http_client_is_complete_data_received(http)) {
                    mp3_stream_close(&http);
                    s_state = LUA_AUDIO_MP3_BUFFERING;
                    continue;
                }
                /* A stalled read is not fatal: the PCM jitter buffer keeps
                 * playing. Reconnect only after a few timeouts. */
                if (++read_timeouts > 2) {
                    mp3_stream_close(&http);
                    read_timeouts = 0;
                    s_state = LUA_AUDIO_MP3_BUFFERING;
                    vTaskDelay(pdMS_TO_TICKS(500));
                } else {
                    if (out.pcm != NULL &&
                        xStreamBufferBytesAvailable(out.pcm) >= prebuffer_bytes / 2) {
                        s_state = LUA_AUDIO_MP3_PLAYING;
                    } else {
                        s_state = LUA_AUDIO_MP3_BUFFERING;
                    }
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                continue;
            }
            read_timeouts = 0;
        } else {
            read_bytes = (int)fread(input, 1, MP3_READ_SIZE, file);
            if (read_bytes <= 0) {
                break;
            }
        }

        esp_audio_simple_dec_raw_t raw = {
            .buffer = input,
            .len = (uint32_t)read_bytes,
            .eos = stream ? false : feof(file),
        };
        esp_audio_simple_dec_out_t frame = {
            .buffer = output,
            .len = MP3_OUT_SIZE,
        };
        while (raw.len > 0 && !s_stop) {
            frame.decoded_size = 0;
            frame.needed_size = 0;
            esp_audio_err_t ret = esp_audio_simple_dec_process(decoder, &raw, &frame);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                if (frame.needed_size <= frame.len) {
                    mp3_set_error("MP3 decode failed");
                    failed = true;
                    goto cleanup;
                }
                uint8_t *grown = heap_caps_realloc(output, frame.needed_size,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (grown == NULL) {
                    mp3_set_error("out of memory");
                    failed = true;
                    goto cleanup;
                }
                output = grown;
                frame.buffer = output;
                frame.len = frame.needed_size;
                continue;
            }
            if (ret != ESP_AUDIO_ERR_OK) {
                if (raw.consumed == 0) {
                    break;
                }
                raw.buffer += raw.consumed;
                raw.len -= raw.consumed;
                continue;
            }
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
            if (frame.decoded_size == 0) {
                if (raw.consumed == 0) {
                    break;
                }
                continue;
            }

            esp_audio_simple_dec_info_t info = {0};
            if (esp_audio_simple_dec_get_info(decoder, &info) != ESP_AUDIO_ERR_OK ||
                info.bits_per_sample != 16 ||
                info.channel == 0 || info.channel > 2 ||
                info.sample_rate < MP3_MIN_RATE || info.sample_rate > MP3_MAX_RATE) {
                mp3_set_error("unsupported MP3 format");
                failed = true;
                goto cleanup;
            }
            if (!exclusive) {
                format_rate = info.sample_rate;
                format_channels = info.channel;
                if (!ai_audio_exclusive_begin((int)format_rate, 2)) {
                    mp3_set_error("assistant busy or audio codec unavailable");
                    failed = true;
                    goto cleanup;
                }
                exclusive = true;
                s_src_rate = format_rate;
                s_src_channels = format_channels;

                size_t capacity = (size_t)format_rate * 4U * MP3_PCM_SECONDS;
                if (capacity > 1024U * 1024U) {
                    capacity = 1024U * 1024U;
                }
                out.pcm = xStreamBufferCreateWithCaps(capacity, 1,
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                out.done = xSemaphoreCreateBinary();
                if (out.pcm == NULL || out.done == NULL) {
                    mp3_set_error("out of memory");
                    failed = true;
                    goto cleanup;
                }
                prebuffer_bytes = (size_t)format_rate * 4U * MP3_PREBUFFER_MS / 1000U;
                if (prebuffer_bytes > capacity / 2) {
                    prebuffer_bytes = capacity / 2;
                }
            } else if (info.sample_rate != format_rate || info.channel != format_channels) {
                mp3_set_error("MP3 format changed mid-stream");
                failed = true;
                goto cleanup;
            }

            size_t frames = frame.decoded_size / (sizeof(int16_t) * info.channel);
            if (frames == 0 || frames > MP3_MAX_SAMPLES) {
                mp3_set_error("MP3 frame too large");
                failed = true;
                goto cleanup;
            }
            decoded_frames++;

            const int16_t *pcm = (const int16_t *)frame.buffer;
            if (info.channel == 1) {
                for (size_t i = 0; i < frames; ++i) {
                    stereo[i * 2] = pcm[i];
                    stereo[i * 2 + 1] = pcm[i];
                }
                pcm = stereo;
            }
            int64_t sum = 0;
            size_t samples = frames * 2;
            for (size_t i = 0; i < samples; ++i) {
                int32_t value = pcm[i];
                sum += (int64_t)value * value;
            }
            float rms = sqrtf((float)sum / (float)samples) / 32768.0f;
            s_level = rms > 1.0f ? 1.0f : rms;

            size_t pcm_bytes = samples * sizeof(int16_t);
            size_t sent = 0;
            while (sent < pcm_bytes && !s_stop && !out.failed) {
                size_t chunk = xStreamBufferSend(out.pcm, (const uint8_t *)pcm + sent,
                                                 pcm_bytes - sent, pdMS_TO_TICKS(100));
                sent += chunk;
                if (!out_started &&
                    xStreamBufferBytesAvailable(out.pcm) >= prebuffer_bytes) {
                    if (!mp3_start_output(&out)) {
                        mp3_set_error("failed to create output task");
                        failed = true;
                        goto cleanup;
                    }
                    out_started = true;
                }
                if (chunk == 0 && !out_started) {
                    if (!mp3_start_output(&out)) {
                        mp3_set_error("failed to create output task");
                        failed = true;
                        goto cleanup;
                    }
                    out_started = true;
                }
            }
            if (out.failed) {
                mp3_set_error(out.error[0] ? out.error : "audio write failed");
                failed = true;
                goto cleanup;
            }
            s_state = out_started ? LUA_AUDIO_MP3_PLAYING : LUA_AUDIO_MP3_BUFFERING;
        }
    }

cleanup:
    if (!out_started && out.pcm != NULL && xStreamBufferBytesAvailable(out.pcm) > 0) {
        if (mp3_start_output(&out)) {
            out_started = true;
        }
    }
    if (out_started) {
        if (s_stop || failed) {
            out.stop = true;
        } else {
            out.producer_done = true;
        }
        (void)xSemaphoreTake(out.done, pdMS_TO_TICKS(5000));
        out.task = NULL;
    }
    if (out.pcm != NULL) {
        mp3_stream_buffer_destroy(out.pcm);
        out.pcm = NULL;
    }
    if (out.done != NULL) {
        vSemaphoreDelete(out.done);
        out.done = NULL;
    }
    s_level = 0;
    mp3_stream_close(&http);
    if (exclusive) {
        ai_audio_exclusive_end();
    }
    if (decoder != NULL) {
        esp_audio_simple_dec_close(decoder);
    }
    heap_caps_free(input);
    heap_caps_free(output);
    heap_caps_free(stereo);
    if (file != NULL) {
        fclose(file);
    }
    if (!s_stop && !failed && !stream && decoded_frames == 0) {
        mp3_set_error("no MP3 audio data");
        failed = true;
    }
    if (s_stop) {
        s_state = LUA_AUDIO_MP3_STOPPED;
    } else if (failed || s_error[0] != '\0') {
        s_state = LUA_AUDIO_MP3_ERROR;
    } else if (stream) {
        s_state = LUA_AUDIO_MP3_ERROR;
    } else {
        s_state = LUA_AUDIO_MP3_FINISHED;
    }
    s_task = NULL;
    xSemaphoreGive(s_done);
    vTaskDeleteWithCaps(NULL);
}

esp_err_t lua_audio_mp3_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_done == NULL) {
        s_done = xSemaphoreCreateBinary();
    }
    return (s_lock != NULL && s_done != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

bool lua_audio_mp3_play(const char *path, uint32_t wait_ms, const char *owner_job)
{
    if (path == NULL || path[0] == '\0' || lua_audio_mp3_init() != ESP_OK) {
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_task != NULL) {
        s_stop = true;
        xSemaphoreGive(s_lock);
        xSemaphoreTake(s_done, pdMS_TO_TICKS(MP3_STOP_WAIT_MS));
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (s_task != NULL) {
        mp3_set_error("player busy");
        xSemaphoreGive(s_lock);
        return false;
    }

    strlcpy(s_path, path, sizeof(s_path));
    strlcpy(s_owner_job, owner_job ? owner_job : "", sizeof(s_owner_job));
    s_is_stream = lua_audio_mp3_is_url(path);
    s_stop = false;
    s_error[0] = '\0';
    s_src_rate = 0;
    s_src_channels = 0;
    s_level = 0;
    s_state = s_is_stream ? LUA_AUDIO_MP3_BUFFERING : LUA_AUDIO_MP3_PLAYING;
    xSemaphoreTake(s_done, 0);
    BaseType_t created = xTaskCreateWithCaps(mp3_task, "lua_mp3", MP3_TASK_STACK,
                                             NULL, MP3_TASK_PRIO, &s_task,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        s_task = NULL;
        s_state = LUA_AUDIO_MP3_ERROR;
        mp3_set_error("failed to create MP3 task");
    }
    xSemaphoreGive(s_lock);
    if (created != pdPASS) {
        return false;
    }
    if (wait_ms > 0) {
        xSemaphoreTake(s_done, pdMS_TO_TICKS(wait_ms));
    }
    return true;
}

void lua_audio_mp3_stop(void)
{
    if (s_lock == NULL) {
        return;
    }
    TaskHandle_t task = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    task = s_task;
    if (task != NULL) {
        s_stop = true;
    }
    xSemaphoreGive(s_lock);
    if (task != NULL) {
        xSemaphoreTake(s_done, pdMS_TO_TICKS(MP3_STOP_WAIT_MS));
    }
    s_owner_job[0] = '\0';
}

void lua_audio_mp3_cleanup_job(const char *job_id)
{
    if (job_id == NULL || job_id[0] == '\0' || s_owner_job[0] == '\0') {
        return;
    }
    if (strcmp(job_id, s_owner_job) == 0) {
        lua_audio_mp3_stop();
    }
}

bool lua_audio_mp3_active(void)
{
    return s_task != NULL;
}

lua_audio_mp3_state_t lua_audio_mp3_state(void)
{
    return s_state;
}

const char *lua_audio_mp3_error(void)
{
    return s_error;
}

uint32_t lua_audio_mp3_source_rate(void)
{
    return s_src_rate;
}

uint8_t lua_audio_mp3_source_channels(void)
{
    return s_src_channels;
}

float lua_audio_mp3_level(void)
{
    return s_level;
}
