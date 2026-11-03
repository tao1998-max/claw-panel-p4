/*
 * Doubao (Volcengine) bidirectional streaming TTS.
 *
 * Protocol: wss://openspeech.bytedance.com/api/v3/tts/bidirection
 * Binary frame: 4-byte header + optional event/session fields + JSON/PCM.
 * We request 16 kHz mono PCM so the ES8311 path can play it directly.
 */
#include "claw_tts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "claw.tts";

#define TTS_WS_URI            "wss://openspeech.bytedance.com/api/v3/tts/bidirection"
#define TTS_NVS_NS            "doubao"
#define TTS_DEFAULT_TIMEOUT   120000
#define TTS_RX_MAX            (64 * 1024)
/* 128 KB ≈ 4 s of 16 kHz mono PCM. Keep this small: with a large buffer the
 * server dumps the whole reply into PSRAM at once, which starves the DPI
 * scanout and shows as a one-frame white flash on this panel. Long replies
 * are kept alive by text-level flow control instead (short TaskRequests
 * paced by a delivered-audio byte credit), so the WebSocket task never
 * blocks long enough to miss pings or trip the peer's idle reset. */
#define TTS_PCM_STREAM_SIZE   (128 * 1024)
#define TTS_QUEUE_TIMEOUT_MS  5000
/* Text-level credit control. `pcm_bytes` counts audio the server has already
 * delivered for the text sent so far; keep the un-delivered remainder below
 * this many bytes, leaving the rest of the 128 KB buffer as headroom for the
 * WebSocket task. Without this, a fast LLM can hand the server the whole
 * reply in seconds, the server then synthesizes tens of seconds ahead, and
 * the WebSocket task blocks on the full PCM buffer until the peer resets. */
#define TTS_FLOW_CREDIT_BYTES (TTS_PCM_STREAM_SIZE / 2)
#define TTS_FLOW_POLL_MS      20
/* Rough PCM bytes produced per spoken character (16 kHz mono). Chinese TTS is
 * ~4-5 chars/s, i.e. 6400-8000 bytes/char; this voice measures ~7000. The
 * empty-buffer fallback below covers voices that speak faster than this. */
#define TTS_EST_BYTES_PER_CHAR 7000
/* Doubao recommends short TaskRequests for streaming synthesis. Short chunks
 * also bound how much audio the server can produce ahead of playback, which
 * is what keeps the WebSocket receive task responsive on long replies. */
#define TTS_TASK_CHARS        40
/* Worker/player stacks are created with xTaskCreateWithCaps() in PSRAM;
 * keeping them out of internal RAM leaves headroom for TLS/Wi-Fi while a
 * reply is being synthesized and played. */
#define TTS_TASK_STACK        8192
/* Mild anti-pop ramp: the first PCM of an utterance starts at zero amplitude
 * and ramps up over 100 ms. The TTS-start display flash was traced to a
 * large PCM buffer burst, not to this amplitude step; keep the ramp only as
 * an audio-start guard. */
#define TTS_FADE_IN_SAMPLES   (16000 / 10)

#define TTS_EV_CONN_STARTED   (1 << 0)
#define TTS_EV_SESSION_START  (1 << 1)
#define TTS_EV_SESSION_DONE   (1 << 2)
#define TTS_EV_AUDIO_DONE     (1 << 3)
#define TTS_EV_ERROR          (1 << 4)

#define TTS_EVENT_START_CONNECTION 1
#define TTS_EVENT_FINISH_CONNECTION 2
#define TTS_EVENT_CONNECTION_STARTED 50
#define TTS_EVENT_CONNECTION_FAILED 51
#define TTS_EVENT_CONNECTION_FINISHED 52
#define TTS_EVENT_START_SESSION 100
#define TTS_EVENT_FINISH_SESSION 102
#define TTS_EVENT_SESSION_STARTED 150
#define TTS_EVENT_SESSION_CANCELED 151
#define TTS_EVENT_SESSION_FINISHED 152
#define TTS_EVENT_SESSION_FAILED 153
#define TTS_EVENT_TASK_REQUEST 200
#define TTS_EVENT_TTS_RESPONSE 352

typedef struct {
    esp_websocket_client_handle_t client;
    EventGroupHandle_t events;
    StreamBufferHandle_t pcm_stream;
    SemaphoreHandle_t player_done;
    TaskHandle_t player_task;
    uint8_t *rx;
    size_t rx_len;
    size_t rx_cap;
    char session_id[40];
    const char *text;
    uint32_t timeout_ms;
    volatile size_t pcm_bytes;
    size_t sent_expected_bytes;
    volatile bool session_done;
    volatile bool failed;
    volatile bool session_sent;
    char error[160];
} tts_ctx_t;

static tts_ctx_t s_ctx;
static claw_tts_config_t s_cfg;
static bool s_cfg_loaded;
static SemaphoreHandle_t s_speak_lock;
static StreamBufferHandle_t s_pcm_stream;
static QueueHandle_t s_stream_queue;
static volatile bool s_stream_active;
static tts_ctx_t s_stream_ctx;
static SemaphoreHandle_t s_cfg_lock;
static QueueHandle_t s_async_queue;
static claw_tts_play_pcm_fn s_play_pcm;
static claw_tts_activity_fn s_activity_cb;
static volatile bool s_active_notified;
static portMUX_TYPE s_flag_mux = portMUX_INITIALIZER_UNLOCKED;

static size_t utf8_sequence_len(unsigned char c);

static bool claim_flag(volatile bool *flag)
{
    taskENTER_CRITICAL(&s_flag_mux);
    bool first = !*flag;
    if (first) {
        *flag = true;
    }
    taskEXIT_CRITICAL(&s_flag_mux);
    return first;
}

static uint32_t read_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void write_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void tts_fail(tts_ctx_t *c, const char *message)
{
    if (c->failed) {
        return;
    }
    c->failed = true;
    strlcpy(c->error, message != NULL ? message : "tts failed", sizeof(c->error));
    xEventGroupSetBits(c->events, TTS_EV_ERROR);
}

/* ---------- config ---------- */

static void nvs_read_str(nvs_handle_t h, const char *key, char *out, size_t out_size)
{
    size_t size = out_size;
    if (nvs_get_str(h, key, out, &size) != ESP_OK) {
        out[0] = '\0';
    }
}

static void config_load_locked(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    strlcpy(s_cfg.resource_id, "seed-tts-2.0", sizeof(s_cfg.resource_id));
    s_cfg.auto_speak = true;

    nvs_handle_t h;
    if (nvs_open(TTS_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_read_str(h, "app_id", s_cfg.app_id, sizeof(s_cfg.app_id));
        nvs_read_str(h, "access_token", s_cfg.access_token,
                     sizeof(s_cfg.access_token));
        nvs_read_str(h, "api_key", s_cfg.api_key, sizeof(s_cfg.api_key));
        nvs_read_str(h, "resource_id", s_cfg.resource_id,
                     sizeof(s_cfg.resource_id));
        nvs_read_str(h, "speaker", s_cfg.speaker, sizeof(s_cfg.speaker));
        uint8_t auto_speak = 1;
        if (nvs_get_u8(h, "auto_speak", &auto_speak) == ESP_OK) {
            s_cfg.auto_speak = auto_speak != 0;
        }
        nvs_close(h);
    }
    if (s_cfg.resource_id[0] == '\0') {
        strlcpy(s_cfg.resource_id, "seed-tts-2.0", sizeof(s_cfg.resource_id));
    }
    s_cfg_loaded = true;
}

static void config_ensure(void)
{
    if (s_cfg_lock != NULL) {
        xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
    }
    if (!s_cfg_loaded) {
        config_load_locked();
    }
    if (s_cfg_lock != NULL) {
        xSemaphoreGive(s_cfg_lock);
    }
}

static bool config_configured(const claw_tts_config_t *cfg)
{
    if (cfg->speaker[0] == '\0') {
        return false;
    }
    return cfg->api_key[0] != '\0' ||
           (cfg->app_id[0] != '\0' && cfg->access_token[0] != '\0');
}

esp_err_t claw_tts_get_config(claw_tts_config_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    config_ensure();
    if (s_cfg_lock != NULL) {
        xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
    }
    *out = s_cfg;
    if (s_cfg_lock != NULL) {
        xSemaphoreGive(s_cfg_lock);
    }
    return ESP_OK;
}

esp_err_t claw_tts_set_config(const claw_tts_config_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_cfg_lock == NULL) {
        s_cfg_lock = xSemaphoreCreateMutex();
        if (s_cfg_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreTake(s_cfg_lock, portMAX_DELAY);
    s_cfg = *cfg;
    if (s_cfg.resource_id[0] == '\0') {
        strlcpy(s_cfg.resource_id, "seed-tts-2.0", sizeof(s_cfg.resource_id));
    }
    s_cfg_loaded = true;

    nvs_handle_t h;
    esp_err_t err = nvs_open(TTS_NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "app_id", s_cfg.app_id);
        if (err == ESP_OK) {
            err = nvs_set_str(h, "access_token", s_cfg.access_token);
        }
        if (err == ESP_OK) {
            err = nvs_set_str(h, "api_key", s_cfg.api_key);
        }
        if (err == ESP_OK) {
            err = nvs_set_str(h, "resource_id", s_cfg.resource_id);
        }
        if (err == ESP_OK) {
            err = nvs_set_str(h, "speaker", s_cfg.speaker);
        }
        if (err == ESP_OK) {
            err = nvs_set_u8(h, "auto_speak", s_cfg.auto_speak ? 1 : 0);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    xSemaphoreGive(s_cfg_lock);
    return err;
}

bool claw_tts_is_configured(void)
{
    claw_tts_config_t cfg;
    if (claw_tts_get_config(&cfg) != ESP_OK) {
        return false;
    }
    return config_configured(&cfg);
}

bool claw_tts_auto_speak_enabled(void)
{
    claw_tts_config_t cfg;
    if (claw_tts_get_config(&cfg) != ESP_OK) {
        return false;
    }
    return cfg.auto_speak && config_configured(&cfg);
}

void claw_tts_init(void)
{
    if (s_speak_lock == NULL) {
        s_speak_lock = xSemaphoreCreateMutex();
    }
    if (s_cfg_lock == NULL) {
        s_cfg_lock = xSemaphoreCreateMutex();
    }
    config_ensure();
}

void claw_tts_set_playback(claw_tts_play_pcm_fn fn)
{
    s_play_pcm = fn;
}

void claw_tts_set_activity_callback(claw_tts_activity_fn fn)
{
    s_activity_cb = fn;
}

/* Only one TTS session can own the speak lock, so a single edge flag is
 * enough. The callback lets main mute the assistant microphone while Claw
 * audio plays, so XiaoZhi cannot hear (and react to) its own reply. */
static void tts_notify_active(bool active)
{
    if (s_active_notified == active) {
        return;
    }
    s_active_notified = active;
    if (s_activity_cb != NULL) {
        s_activity_cb(active);
    }
}

/* ---------- frames ---------- */

static size_t build_frame(uint8_t *out, size_t cap, int event,
                          const char *session_id, const char *payload,
                          size_t payload_len)
{
    size_t need = 4 + 4;
    if (session_id != NULL) {
        need += 4 + strlen(session_id);
    }
    need += 4 + payload_len;
    if (need > cap) {
        return 0;
    }

    size_t off = 0;
    out[off++] = 0x11;               /* protocol v1, header size 4 */
    out[off++] = 0x14;               /* full client request, with event */
    out[off++] = 0x10;               /* JSON, no compression */
    out[off++] = 0x00;
    write_u32(out + off, (uint32_t)event);
    off += 4;
    if (session_id != NULL) {
        size_t len = strlen(session_id);
        write_u32(out + off, (uint32_t)len);
        off += 4;
        memcpy(out + off, session_id, len);
        off += len;
    }
    write_u32(out + off, (uint32_t)payload_len);
    off += 4;
    if (payload_len > 0) {
        memcpy(out + off, payload, payload_len);
        off += payload_len;
    }
    return off;
}

static esp_err_t send_event(tts_ctx_t *c, int event, const char *session_id,
                            const char *payload)
{
    if (c->client == NULL || !esp_websocket_client_is_connected(c->client)) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t payload_len = payload != NULL ? strlen(payload) : 0;
    size_t cap = 4 + 4 + 4 + 40 + 4 + payload_len + 8;
    uint8_t *frame = malloc(cap);
    if (frame == NULL) {
        return ESP_ERR_NO_MEM;
    }
    size_t len = build_frame(frame, cap, event, session_id, payload, payload_len);
    if (len == 0) {
        free(frame);
        return ESP_ERR_INVALID_SIZE;
    }
    int sent = esp_websocket_client_send_bin(c->client, (const char *)frame,
                                             (int)len, pdMS_TO_TICKS(2000));
    free(frame);
    if (sent < 0 || (size_t)sent != len) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static char *make_payload(const char *text, int event, bool with_params)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "namespace", "BidirectionalTTS");
    cJSON_AddNumberToObject(root, "event", event);
    cJSON *user = cJSON_AddObjectToObject(root, "user");
    if (user != NULL) {
        cJSON_AddStringToObject(user, "uid", "esp32p4");
    }

    if (with_params) {
        claw_tts_config_t cfg;
        (void)claw_tts_get_config(&cfg);
        cJSON *params = cJSON_AddObjectToObject(root, "req_params");
        if (params != NULL) {
            cJSON_AddStringToObject(params, "text", text != NULL ? text : "");
            cJSON_AddStringToObject(params, "speaker", cfg.speaker);
            cJSON *audio = cJSON_AddObjectToObject(params, "audio_params");
            if (audio != NULL) {
                cJSON_AddStringToObject(audio, "format", "pcm");
                cJSON_AddNumberToObject(audio, "sample_rate", 16000);
                cJSON_AddNumberToObject(audio, "speech_rate", 0);
                cJSON_AddNumberToObject(audio, "loudness_rate", 0);
            }
            cJSON_AddStringToObject(params, "additions",
                                    "{\"disable_markdown_filter\":true}");
        }
    }

    char *text_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text_json;
}

static void start_session(tts_ctx_t *c)
{
    if (!claim_flag(&c->session_sent)) {
        return;
    }
    char *payload = make_payload(NULL, TTS_EVENT_START_SESSION, true);
    if (payload == NULL) {
        tts_fail(c, "no memory for session payload");
        return;
    }
    esp_err_t err = send_event(c, TTS_EVENT_START_SESSION, c->session_id, payload);
    cJSON_free(payload);
    if (err != ESP_OK) {
        tts_fail(c, "start session send failed");
    }
}

static esp_err_t send_task_text(tts_ctx_t *c, const char *text)
{
    char *payload = make_payload(text, TTS_EVENT_TASK_REQUEST, true);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = send_event(c, TTS_EVENT_TASK_REQUEST, c->session_id,
                               payload);
    cJSON_free(payload);
    return err;
}

/* Copy at most max_chars UTF-8 characters into out (NUL terminated), return
 * the number of bytes consumed and store the character count. Never splits a
 * UTF-8 sequence. */
static size_t utf8_take(const char *text, size_t max_chars, char *out,
                        size_t out_size, size_t *out_chars)
{
    size_t i = 0;
    size_t chars = 0;
    size_t o = 0;

    while (text[i] != '\0' && chars < max_chars) {
        size_t n = utf8_sequence_len((unsigned char)text[i]);
        if (n > 1 && text[i + 1] == '\0') {
            break;
        }
        if (o + n >= out_size) {
            break;
        }
        memcpy(out + o, text + i, n);
        i += n;
        o += n;
        chars++;
    }
    out[o] = '\0';
    if (out_chars != NULL) {
        *out_chars = chars;
    }
    return i;
}

/* Wait until the server has delivered enough audio for the text already sent
 * that the remaining in-flight audio fits in the buffer headroom. This is a
 * byte credit, not a buffer-level check: it also accounts for text the server
 * has accepted but not yet turned into audio (the failure mode where the LLM
 * hands over a whole reply and the server synthesizes far ahead). */
static bool tts_wait_credit(tts_ctx_t *c, int64_t deadline)
{
    bool waiting = false;
    size_t last_received = 0;
    int64_t empty_since = 0;

    for (;;) {
        if (c->failed) {
            return false;
        }
        size_t received = c->pcm_bytes;
        size_t expected = c->sent_expected_bytes;
        size_t lead = expected > received ? expected - received : 0;
        if (lead <= TTS_FLOW_CREDIT_BYTES) {
            if (waiting) {
                ESP_LOGD(TAG, "flow resume (lead=%u)", (unsigned)lead);
            }
            return true;
        }

        /* If the playback buffer is empty and no audio has arrived for a
         * while, the server has already delivered everything for the text
         * sent (a voice that speaks faster than the byte estimate). Let the
         * producer continue instead of waiting for audio that will never
         * come. A blocked WebSocket task has a full buffer, so this cannot
         * mask real backpressure. */
        if (received != last_received) {
            last_received = received;
            empty_since = 0;
        } else if (xStreamBufferBytesAvailable(c->pcm_stream) == 0) {
            int64_t now = esp_timer_get_time();
            if (empty_since == 0) {
                empty_since = now;
            } else if (now - empty_since > 300000) {
                ESP_LOGD(TAG, "flow fallback: buffer empty, server caught up");
                return true;
            }
        } else {
            empty_since = 0;
        }

        if (!waiting) {
            ESP_LOGD(TAG, "flow wait (lead=%u received=%u)",
                     (unsigned)lead, (unsigned)received);
            waiting = true;
        }
        if (esp_timer_get_time() > deadline) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(TTS_FLOW_POLL_MS));
    }
}

/* Split a sentence into short TaskRequests, respecting the audio-delivery
 * credit before each one. This keeps the WebSocket receive task from blocking
 * for the whole reply (which made the server reset long sessions after ~60 s)
 * while retaining the small 128 KB buffer that avoids the display flash. */
static bool tts_send_text_paced(tts_ctx_t *c, const char *text,
                                int64_t deadline)
{
    size_t len = strlen(text);
    size_t off = 0;
    char chunk[TTS_TASK_CHARS * 4 + 1];

    while (off < len) {
        if (!tts_wait_credit(c, deadline)) {
            if (!c->failed) {
                tts_fail(c, "stream flow timeout");
            }
            return false;
        }
        size_t chars = 0;
        size_t used = utf8_take(text + off, TTS_TASK_CHARS, chunk,
                                sizeof(chunk), &chars);
        if (used == 0 || chars == 0) {
            break;
        }
        c->sent_expected_bytes += chars * TTS_EST_BYTES_PER_CHAR;
        if (send_task_text(c, chunk) != ESP_OK) {
            tts_fail(c, "stream text send failed");
            return false;
        }
        off += used;
    }
    return true;
}

static void queue_pcm(tts_ctx_t *c, const uint8_t *data, size_t len)
{
    if (len == 0 || c->pcm_stream == NULL) {
        return;
    }
    size_t sent = xStreamBufferSend(c->pcm_stream, data, len,
                                    pdMS_TO_TICKS(TTS_QUEUE_TIMEOUT_MS));
    c->pcm_bytes += sent;
    if (sent != len) {
        tts_fail(c, "audio queue full");
    }
}

/* Returns bytes consumed, 0 when the buffer holds an incomplete frame, or
 * (size_t)-1 on a malformed frame. */
static size_t handle_frame(tts_ctx_t *c, const uint8_t *buf, size_t len)
{
    if (len < 4) {
        return 0;
    }
    uint8_t msg_type = (buf[1] >> 4) & 0x0F;
    uint8_t flags = buf[1] & 0x0F;
    size_t off = 4;

    if (msg_type == 0x0F) {  /* error information */
        if (len < 8) {
            return 0;
        }
        uint32_t code = read_u32(buf + 4);
        if (len < 12) {
            return 0;
        }
        uint32_t payload_len = read_u32(buf + 8);
        if (len < 12 + payload_len) {
            return 0;
        }
        char msg[96];
        snprintf(msg, sizeof(msg), "server error %lu", (unsigned long)code);
        tts_fail(c, msg);
        return 12 + payload_len;
    }

    if ((msg_type != 0x09 && msg_type != 0x0B) || (flags & 0x04) == 0) {
        /* Unknown/unneeded frame type. Drop it to avoid a parse loop. */
        return len;
    }
    if (len < 8) {
        return 0;
    }
    uint32_t event = read_u32(buf + 4);
    off = 8;

    if (event == TTS_EVENT_CONNECTION_STARTED ||
        event == TTS_EVENT_CONNECTION_FAILED) {
        if (len < off + 4) {
            return 0;
        }
        uint32_t content_len = read_u32(buf + off);
        if (len < off + 4 + content_len) {
            return 0;
        }
        if (event == TTS_EVENT_CONNECTION_STARTED) {
            xEventGroupSetBits(c->events, TTS_EV_CONN_STARTED);
            start_session(c);
        } else {
            tts_fail(c, "connection failed");
        }
        return off + 4 + content_len;
    }

    if (event == TTS_EVENT_CONNECTION_FINISHED) {
        return off;
    }

    if (event == TTS_EVENT_SESSION_STARTED ||
        event == TTS_EVENT_SESSION_FAILED ||
        event == TTS_EVENT_SESSION_FINISHED ||
        event == TTS_EVENT_SESSION_CANCELED) {
        /* Two content blocks: session id and JSON. */
        for (int i = 0; i < 2; i++) {
            if (len < off + 4) {
                return 0;
            }
            uint32_t content_len = read_u32(buf + off);
            if (len < off + 4 + content_len) {
                return 0;
            }
            off += 4 + content_len;
        }
        if (event == TTS_EVENT_SESSION_STARTED) {
            xEventGroupSetBits(c->events, TTS_EV_SESSION_START);
        } else if (event == TTS_EVENT_SESSION_FINISHED) {
            c->session_done = true;
            xEventGroupSetBits(c->events, TTS_EV_SESSION_DONE);
        } else {
            tts_fail(c, event == TTS_EVENT_SESSION_CANCELED
                        ? "session canceled" : "session failed");
        }
        return off;
    }

    /* TTSResponse and sentence events: session id then payload. */
    if (len < off + 4) {
        return 0;
    }
    uint32_t sid_len = read_u32(buf + off);
    if (len < off + 4 + sid_len) {
        return 0;
    }
    off += 4 + sid_len;
    if (len < off + 4) {
        return 0;
    }
    uint32_t payload_len = read_u32(buf + off);
    off += 4;
    if (len < off + payload_len) {
        return 0;
    }
    if (event == TTS_EVENT_TTS_RESPONSE) {
        queue_pcm(c, buf + off, payload_len);
    }
    return off + payload_len;
}

static void player_task(void *arg)
{
    tts_ctx_t *c = arg;
    uint8_t tmp[2048];
    uint8_t buf[2050];
    uint8_t pending = 0;
    bool has_pending = false;
    size_t fade_left = TTS_FADE_IN_SAMPLES;
    bool first_pcm = true;

    while (!c->failed) {
        size_t n = xStreamBufferReceive(c->pcm_stream, tmp, sizeof(tmp),
                                        pdMS_TO_TICKS(200));
        size_t total = 0;
        if (has_pending) {
            buf[total++] = pending;
        }
        if (n > 0) {
            memcpy(buf + total, tmp, n);
            total += n;
        }
        size_t even = total & ~(size_t)1;
        if (even > 0) {
            int16_t *pcm = (int16_t *)buf;
            size_t samples = even / 2;
            if (first_pcm) {
                ESP_LOGI(TAG, "first pcm %u samples", (unsigned)samples);
                first_pcm = false;
            }
            for (size_t i = 0; i < samples && fade_left > 0; i++, fade_left--) {
                int32_t scaled = (int32_t)pcm[i] *
                                 (int32_t)(TTS_FADE_IN_SAMPLES - fade_left) /
                                 (int32_t)TTS_FADE_IN_SAMPLES;
                pcm[i] = (int16_t)scaled;
            }
            if (s_play_pcm == NULL ||
                !s_play_pcm((const int16_t *)pcm, samples, 2000)) {
                tts_fail(c, "audio playback failed");
                break;
            }
        }
        if (total & 1) {
            pending = buf[total - 1];
            has_pending = true;
        } else {
            has_pending = false;
        }
        if (c->session_done && xStreamBufferIsEmpty(c->pcm_stream)) {
            xEventGroupSetBits(c->events, TTS_EV_AUDIO_DONE);
            break;
        }
    }
    if (c->player_done != NULL) {
        xSemaphoreGive(c->player_done);
    }
    vTaskDeleteWithCaps(NULL);
}

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t event_id,
                             void *event_data)
{
    (void)base;
    tts_ctx_t *c = arg;

    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        if (send_event(c, TTS_EVENT_START_CONNECTION, NULL, "{}") != ESP_OK) {
            tts_fail(c, "start connection send failed");
        }
        return;
    }
    if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        if (!c->session_done) {
            tts_fail(c, "websocket disconnected");
        }
        return;
    }
    if (event_id == WEBSOCKET_EVENT_ERROR) {
        tts_fail(c, "websocket error");
        return;
    }
    if (event_id != WEBSOCKET_EVENT_DATA || event_data == NULL) {
        return;
    }

    esp_websocket_event_data_t *ev = event_data;
    if (ev->op_code != 0x02 && ev->op_code != 0x00) {
        return;
    }
    if (ev->data_len == 0) {
        return;
    }
    if (c->rx_len + (size_t)ev->data_len > c->rx_cap) {
        tts_fail(c, "rx buffer overflow");
        return;
    }
    memcpy(c->rx + c->rx_len, ev->data_ptr, (size_t)ev->data_len);
    c->rx_len += (size_t)ev->data_len;

    size_t off = 0;
    while (off < c->rx_len) {
        size_t used = handle_frame(c, c->rx + off, c->rx_len - off);
        if (used == 0) {
            break;
        }
        if (used == (size_t)-1) {
            tts_fail(c, "malformed frame");
            off = c->rx_len;
            break;
        }
        off += used;
    }
    if (off > 0 && off <= c->rx_len) {
        memmove(c->rx, c->rx + off, c->rx_len - off);
        c->rx_len -= off;
    }
}

static void ensure_init(void)
{
    claw_tts_init();
}

static esp_err_t tts_speak_once(const char *text, uint32_t timeout_ms)
{
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    claw_tts_config_t cfg;
    (void)claw_tts_get_config(&cfg);

    if (timeout_ms == 0) {
        timeout_ms = TTS_DEFAULT_TIMEOUT;
    }
    ESP_LOGI(TAG, "speak %u bytes", (unsigned)strlen(text));
    tts_notify_active(true);
    int64_t started_us = esp_timer_get_time();
    int64_t deadline = started_us + (int64_t)timeout_ms * 1000;

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.timeout_ms = timeout_ms;
    s_ctx.text = text;
    s_ctx.events = xEventGroupCreate();
    if (s_pcm_stream == NULL) {
        s_pcm_stream = xStreamBufferCreateWithCaps(
            TTS_PCM_STREAM_SIZE, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    s_ctx.pcm_stream = s_pcm_stream;
    if (s_ctx.pcm_stream != NULL) {
        xStreamBufferReset(s_ctx.pcm_stream);
    }
    s_ctx.player_done = xSemaphoreCreateBinary();
    s_ctx.rx_cap = TTS_RX_MAX;
    s_ctx.rx = heap_caps_malloc(s_ctx.rx_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ctx.events == NULL || s_ctx.pcm_stream == NULL ||
        s_ctx.player_done == NULL || s_ctx.rx == NULL) {
        tts_fail(&s_ctx, "no memory for tts context");
    }

    snprintf(s_ctx.session_id, sizeof(s_ctx.session_id),
             "%08lx%08lx", (unsigned long)esp_random(),
             (unsigned long)esp_random());

    if (!s_ctx.failed) {
        char headers[512];
        char connect_id[40];
        snprintf(connect_id, sizeof(connect_id), "%08lx%08lx%08lx%08lx",
                 (unsigned long)esp_random(), (unsigned long)esp_random(),
                 (unsigned long)esp_random(), (unsigned long)esp_random());
        if (cfg.api_key[0] != '\0') {
            snprintf(headers, sizeof(headers),
                     "X-Api-Key: %s\r\nX-Api-Resource-Id: %s\r\n"
                     "X-Api-Connect-Id: %s\r\n",
                     cfg.api_key, cfg.resource_id, connect_id);
        } else {
            snprintf(headers, sizeof(headers),
                     "X-Api-App-Key: %s\r\nX-Api-Access-Key: %s\r\n"
                     "X-Api-Resource-Id: %s\r\nX-Api-Connect-Id: %s\r\n",
                     cfg.app_id, cfg.access_token, cfg.resource_id, connect_id);
        }
        esp_websocket_client_config_t ws = {
            .uri = TTS_WS_URI,
            .headers = headers,
            .buffer_size = 8192,
            .task_stack = TTS_TASK_STACK,
            .network_timeout_ms = 10000,
            .disable_auto_reconnect = true,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        s_ctx.client = esp_websocket_client_init(&ws);
        if (s_ctx.client == NULL) {
            tts_fail(&s_ctx, "websocket init failed");
        } else {
            esp_websocket_register_events(s_ctx.client,
                                                 WEBSOCKET_EVENT_ANY,
                                                 ws_event_handler, &s_ctx);
            if (esp_websocket_client_start(s_ctx.client) != ESP_OK) {
                tts_fail(&s_ctx, "websocket start failed");
            }
        }
    }

    if (!s_ctx.failed) {
        BaseType_t ok = xTaskCreateWithCaps(
            player_task, "doubao_player", 8192, &s_ctx, 5,
            &s_ctx.player_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ok != pdPASS) {
            s_ctx.player_task = NULL;
            tts_fail(&s_ctx, "player task failed");
        }
    }

    if (!s_ctx.failed) {
        int64_t remaining = deadline - esp_timer_get_time();
        if (remaining < 0) remaining = 0;
        TickType_t wait_ticks = pdMS_TO_TICKS(1500);
        if (remaining / 1000 < 1500) {
            wait_ticks = pdMS_TO_TICKS(remaining / 1000);
        }
        EventBits_t bits = xEventGroupWaitBits(
            s_ctx.events, TTS_EV_CONN_STARTED | TTS_EV_ERROR,
            pdFALSE, pdFALSE, wait_ticks);
        if ((bits & TTS_EV_CONN_STARTED) == 0 && (bits & TTS_EV_ERROR) == 0) {
            /* Some servers accept StartSession without an explicit
             * StartConnection. Try it once before giving up. */
            start_session(&s_ctx);
        }
    }

    if (!s_ctx.failed) {
        int64_t remaining = deadline - esp_timer_get_time();
        if (remaining < 0) remaining = 0;
        EventBits_t bits = xEventGroupWaitBits(
            s_ctx.events, TTS_EV_SESSION_START | TTS_EV_ERROR,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(remaining / 1000));
        if ((bits & TTS_EV_ERROR) == 0 && (bits & TTS_EV_SESSION_START) == 0) {
            tts_fail(&s_ctx, "session start timeout");
        }
    }

    if (!s_ctx.failed &&
        tts_send_text_paced(&s_ctx, s_ctx.text, deadline)) {
        if (send_event(&s_ctx, TTS_EVENT_FINISH_SESSION, s_ctx.session_id,
                       "{}") != ESP_OK) {
            tts_fail(&s_ctx, "finish session send failed");
        }
    }

    if (!s_ctx.failed) {
        int64_t remaining = deadline - esp_timer_get_time();
        if (remaining < 0) remaining = 0;
        EventBits_t bits = xEventGroupWaitBits(
            s_ctx.events, TTS_EV_AUDIO_DONE | TTS_EV_ERROR,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(remaining / 1000));
        if ((bits & TTS_EV_ERROR) == 0 && (bits & TTS_EV_AUDIO_DONE) == 0) {
            tts_fail(&s_ctx, "audio timeout");
        }
    }

    esp_err_t result = s_ctx.failed ? ESP_FAIL : ESP_OK;
    int64_t elapsed_ms = (esp_timer_get_time() - started_us) / 1000;
    if (s_ctx.failed) {
        ESP_LOGW(TAG, "speak failed: %s (%u bytes, %lld ms)", s_ctx.error,
                 (unsigned)s_ctx.pcm_bytes, (long long)elapsed_ms);
    } else {
        ESP_LOGI(TAG, "speak done (%u bytes, %lld ms)",
                 (unsigned)s_ctx.pcm_bytes, (long long)elapsed_ms);
    }

    if (s_ctx.player_task != NULL) {
        if (xSemaphoreTake(s_ctx.player_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
            vTaskDeleteWithCaps(s_ctx.player_task);
        }
        s_ctx.player_task = NULL;
    }
    if (s_ctx.client != NULL) {
        esp_websocket_client_stop(s_ctx.client);
        esp_websocket_client_destroy(s_ctx.client);
        s_ctx.client = NULL;
    }
    /* The PCM stream is allocated once in PSRAM and reused. Deleting a
     * WithCaps stream buffer trips a TLSF double-free in this IDF build, and
     * keeping it also avoids a repeated alloc/free per utterance. */
    s_ctx.pcm_stream = NULL;
    if (s_ctx.events != NULL) vEventGroupDelete(s_ctx.events);
    if (s_ctx.player_done != NULL) vSemaphoreDelete(s_ctx.player_done);
    if (s_ctx.rx != NULL) heap_caps_free(s_ctx.rx);
    memset(&s_ctx, 0, sizeof(s_ctx));
    tts_notify_active(false);

    return result;
}

static void tts_drain_stream_queue(QueueHandle_t queue)
{
    char *leftover = NULL;
    while (xQueueReceive(queue, &leftover, 0) == pdTRUE) {
        if (leftover != NULL) {
            heap_caps_free(leftover);
        }
    }
}

static void tts_stream_run(QueueHandle_t queue)
{
    claw_tts_config_t cfg;
    (void)claw_tts_get_config(&cfg);
    tts_ctx_t *c = &s_stream_ctx;
    const uint32_t timeout_ms = 600000;
    tts_notify_active(true);
    int64_t started_us = esp_timer_get_time();
    int64_t deadline = started_us + (int64_t)timeout_ms * 1000;
    int64_t start_deadline = started_us + 10000 * 1000;

    if (c->events == NULL) {
        c->events = xEventGroupCreate();
    }
    if (c->player_done == NULL) {
        c->player_done = xSemaphoreCreateBinary();
    }
    if (c->rx == NULL) {
        c->rx_cap = TTS_RX_MAX;
        c->rx = heap_caps_malloc(c->rx_cap,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_pcm_stream == NULL) {
        s_pcm_stream = xStreamBufferCreateWithCaps(
            TTS_PCM_STREAM_SIZE, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    c->pcm_stream = s_pcm_stream;
    if (c->events == NULL || c->player_done == NULL || c->rx == NULL ||
        c->pcm_stream == NULL) {
        tts_fail(c, "no memory for tts stream");
    }

    if (c->events != NULL) {
        /* Event groups reserve the top 8 bits for the kernel. */
        xEventGroupClearBits(c->events,
                             TTS_EV_CONN_STARTED | TTS_EV_SESSION_START |
                             TTS_EV_SESSION_DONE | TTS_EV_AUDIO_DONE |
                             TTS_EV_ERROR);
    }
    while (xSemaphoreTake(c->player_done, 0) == pdTRUE) {
    }
    if (c->pcm_stream != NULL) {
        xStreamBufferReset(c->pcm_stream);
    }
    c->failed = false;
    c->session_done = false;
    c->session_sent = false;
    c->text = NULL;
    c->timeout_ms = timeout_ms;
    c->pcm_bytes = 0;
    c->sent_expected_bytes = 0;
    c->rx_len = 0;
    c->error[0] = '\0';
    snprintf(c->session_id, sizeof(c->session_id),
             "%08lx%08lx", (unsigned long)esp_random(),
             (unsigned long)esp_random());

    bool reuse = c->client != NULL &&
                 esp_websocket_client_is_connected(c->client);
    if (!reuse && c->client != NULL) {
        esp_websocket_client_stop(c->client);
        esp_websocket_client_destroy(c->client);
        c->client = NULL;
    }

    if (!reuse) {
        char headers[512];
        char connect_id[40];
        snprintf(connect_id, sizeof(connect_id), "%08lx%08lx%08lx%08lx",
                 (unsigned long)esp_random(), (unsigned long)esp_random(),
                 (unsigned long)esp_random(), (unsigned long)esp_random());
        if (cfg.api_key[0] != '\0') {
            snprintf(headers, sizeof(headers),
                     "X-Api-Key: %s\r\nX-Api-Resource-Id: %s\r\n"
                     "X-Api-Connect-Id: %s\r\n",
                     cfg.api_key, cfg.resource_id, connect_id);
        } else {
            snprintf(headers, sizeof(headers),
                     "X-Api-App-Key: %s\r\nX-Api-Access-Key: %s\r\n"
                     "X-Api-Resource-Id: %s\r\nX-Api-Connect-Id: %s\r\n",
                     cfg.app_id, cfg.access_token, cfg.resource_id, connect_id);
        }
        esp_websocket_client_config_t ws = {
            .uri = TTS_WS_URI,
            .headers = headers,
            .buffer_size = 8192,
            .task_stack = TTS_TASK_STACK,
            .network_timeout_ms = 10000,
            .disable_auto_reconnect = true,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        c->client = esp_websocket_client_init(&ws);
        if (c->client == NULL) {
            tts_fail(c, "websocket init failed");
        } else {
            esp_websocket_register_events(c->client, WEBSOCKET_EVENT_ANY,
                                          ws_event_handler, c);
            if (esp_websocket_client_start(c->client) != ESP_OK) {
                tts_fail(c, "websocket start failed");
            }
        }
    }

    if (!c->failed) {
        BaseType_t ok = xTaskCreateWithCaps(
            player_task, "doubao_player", 8192, c, 5,
            &c->player_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ok != pdPASS) {
            c->player_task = NULL;
            tts_fail(c, "player task failed");
        }
    }

    if (!c->failed && !reuse) {
        EventBits_t bits = xEventGroupWaitBits(
            c->events, TTS_EV_CONN_STARTED | TTS_EV_ERROR,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
        if ((bits & TTS_EV_CONN_STARTED) == 0 && (bits & TTS_EV_ERROR) == 0) {
            tts_fail(c, "stream connect timeout");
        }
    }

    if (!c->failed && reuse) {
        start_session(c);
    }

    if (!c->failed) {
        int64_t remaining = start_deadline - esp_timer_get_time();
        if (remaining < 0) remaining = 0;
        EventBits_t bits = xEventGroupWaitBits(
            c->events, TTS_EV_SESSION_START | TTS_EV_ERROR,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(remaining / 1000));
        if ((bits & TTS_EV_ERROR) == 0 && (bits & TTS_EV_SESSION_START) == 0) {
            tts_fail(c, "stream session start timeout");
        }
    }

    if (!c->failed) {
        for (;;) {
            char *segment = NULL;
            if (xQueueReceive(queue, &segment, pdMS_TO_TICKS(1000)) == pdTRUE) {
                if (segment == NULL) {
                    (void)send_event(c, TTS_EVENT_FINISH_SESSION,
                                     c->session_id, "{}");
                    break;
                }
                if (!tts_send_text_paced(c, segment, deadline)) {
                    heap_caps_free(segment);
                    break;
                }
                heap_caps_free(segment);
            }
            if (esp_timer_get_time() > deadline) {
                tts_fail(c, "stream timeout");
                break;
            }
        }
    }

    if (!c->failed) {
        int64_t remaining = deadline - esp_timer_get_time();
        if (remaining < 0) remaining = 0;
        EventBits_t bits = xEventGroupWaitBits(
            c->events, TTS_EV_AUDIO_DONE | TTS_EV_ERROR,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(remaining / 1000));
        if ((bits & TTS_EV_ERROR) == 0 && (bits & TTS_EV_AUDIO_DONE) == 0) {
            tts_fail(c, "stream audio timeout");
        }
    }

    esp_err_t result = c->failed ? ESP_FAIL : ESP_OK;
    int64_t elapsed_ms = (esp_timer_get_time() - started_us) / 1000;
    if (result == ESP_OK) {
        ESP_LOGI(TAG, "stream done (%u bytes, %lld ms)",
                 (unsigned)c->pcm_bytes, (long long)elapsed_ms);
    } else {
        ESP_LOGW(TAG, "stream failed: %s (%u bytes, %lld ms)", c->error,
                 (unsigned)c->pcm_bytes, (long long)elapsed_ms);
    }

    if (c->player_task != NULL) {
        if (xSemaphoreTake(c->player_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
            vTaskDeleteWithCaps(c->player_task);
        }
        c->player_task = NULL;
    }

    if (c->failed && c->client != NULL) {
        esp_websocket_client_stop(c->client);
        esp_websocket_client_destroy(c->client);
        c->client = NULL;
    }
    c->session_done = false;
    c->session_sent = false;
    tts_drain_stream_queue(queue);
    tts_notify_active(false);
}

static void tts_stream_task(void *arg)
{
    QueueHandle_t queue = (QueueHandle_t)arg;

    /* A FreeRTOS mutex must be given by the task that took it. The stream
     * worker owns the TTS lock for the whole session; begin() only creates
     * the worker. */
    if (s_speak_lock == NULL ||
        xSemaphoreTake(s_speak_lock, portMAX_DELAY) != pdTRUE) {
        tts_drain_stream_queue(queue);
        s_stream_active = false;
        vTaskDeleteWithCaps(NULL);
        return;
    }
    tts_stream_run(queue);
    xSemaphoreGive(s_speak_lock);
    s_stream_active = false;
    vTaskDeleteWithCaps(NULL);
}

static void tts_sanitize_text(const char *in, char *out, size_t out_size);

esp_err_t claw_tts_stream_begin(void)
{
    if (s_stream_active) {
        return ESP_ERR_INVALID_STATE;
    }
    ensure_init();
    if (!claw_tts_auto_speak_enabled()) {
        return ESP_ERR_INVALID_STATE;
    }
    claw_tts_config_t cfg;
    (void)claw_tts_get_config(&cfg);
    if (!config_configured(&cfg) || s_play_pcm == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_stream_queue == NULL) {
        /* Text-level pacing keeps the worker busy while the LLM streams, so
         * allow the whole reply's sentences to queue up without dropping. */
        s_stream_queue = xQueueCreate(64, sizeof(char *));
    }
    if (s_stream_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_stream_active = true;
    if (xTaskCreateWithCaps(tts_stream_task, "doubao_stream", TTS_TASK_STACK,
                            s_stream_queue, 4, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_stream_active = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "stream begin");
    return ESP_OK;
}

esp_err_t claw_tts_stream_feed(const char *text)
{
    if (!s_stream_active || s_stream_queue == NULL ||
        text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    size_t len = strlen(text);
    char *clean = heap_caps_malloc(len + 1,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (clean == NULL) {
        return ESP_ERR_NO_MEM;
    }
    tts_sanitize_text(text, clean, len + 1);
    if (clean[0] == '\0') {
        heap_caps_free(clean);
        return ESP_OK;
    }
    if (xQueueSend(s_stream_queue, &clean, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "stream feed queue full, dropping %u bytes",
                 (unsigned)len);
        heap_caps_free(clean);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t claw_tts_stream_end(void)
{
    if (!s_stream_active || s_stream_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    char *sentinel = NULL;
    if (xQueueSend(s_stream_queue, &sentinel, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static size_t utf8_sequence_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if (c < 0xE0) return 2;
    if (c < 0xF0) return 3;
    if (c < 0xF8) return 4;
    return 1;
}

/* Drop emoji and pictographs. Doubao TTS rejects them with a generic failure,
 * which used to silently disable auto-speak for replies containing 👋/😊. */
static void tts_sanitize_text(const char *in, char *out, size_t out_size)
{
    const unsigned char *p = (const unsigned char *)in;
    size_t o = 0;
    while (*p && o + 1 < out_size) {
        size_t n = utf8_sequence_len(*p);
        if (p[1] == 0 && n > 1) {
            break;
        }
        bool skip = false;
        if (n == 4) {
            skip = true; /* emoji, flags and rare CJK extensions */
        } else if (n == 3) {
            unsigned cp = ((unsigned)p[0] & 0x0F) << 12 |
                          ((unsigned)p[1] & 0x3F) << 6 |
                          ((unsigned)p[2] & 0x3F);
            if ((cp >= 0x2600 && cp <= 0x27BF) ||  /* misc symbols/dingbats */
                (cp >= 0x2B00 && cp <= 0x2BFF) ||
                (cp >= 0xFE00 && cp <= 0xFE0F) ||  /* variation selectors */
                cp == 0x200D || cp == 0x20E3) {    /* ZWJ / combining keycap */
                skip = true;
            }
        }
        if (skip) {
            p += n;
            continue;
        }
        if (o + n >= out_size) {
            break;
        }
        memcpy(out + o, p, n);
        o += n;
        p += n;
    }
    out[o] = '\0';
    while (o > 0 && out[o - 1] == ' ') {
        out[--o] = '\0';
    }
}

esp_err_t claw_tts_speak(const char *text, uint32_t timeout_ms)
{
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    ensure_init();
    claw_tts_config_t cfg;
    (void)claw_tts_get_config(&cfg);
    if (!config_configured(&cfg) || s_play_pcm == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t text_len = strlen(text);
    char *clean = heap_caps_malloc(text_len + 1,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (clean == NULL) {
        return ESP_ERR_NO_MEM;
    }
    tts_sanitize_text(text, clean, text_len + 1);
    if (clean[0] == '\0') {
        heap_caps_free(clean);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_speak_lock == NULL ||
        xSemaphoreTake(s_speak_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        heap_caps_free(clean);
        return ESP_ERR_INVALID_STATE;
    }
    if (timeout_ms == 0) {
        timeout_ms = TTS_DEFAULT_TIMEOUT;
    }
    esp_err_t result = tts_speak_once(clean, timeout_ms);
    xSemaphoreGive(s_speak_lock);
    heap_caps_free(clean);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "speak failed: %s", esp_err_to_name(result));
    }
    return result;
}

static uint32_t auto_speak_timeout(const char *text)
{
    size_t bytes = strlen(text);
    uint64_t ms = 20000 + (uint64_t)bytes * 100;
    if (ms < TTS_DEFAULT_TIMEOUT) {
        ms = TTS_DEFAULT_TIMEOUT;
    }
    if (ms > 600000) {
        ms = 600000;
    }
    return (uint32_t)ms;
}

/* ---------- auto-speak worker ---------- */

static void speak_worker(void *arg)
{
    (void)arg;
    char *text = NULL;
    for (;;) {
        if (xQueueReceive(s_async_queue, &text, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (text != NULL) {
            (void)claw_tts_speak(text, auto_speak_timeout(text));
            heap_caps_free(text);
        }
    }
}

esp_err_t claw_tts_speak_async(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_tts_auto_speak_enabled()) {
        return ESP_ERR_INVALID_STATE;
    }
    ensure_init();
    if (s_async_queue == NULL) {
        s_async_queue = xQueueCreate(32, sizeof(char *));
        if (s_async_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
        if (xTaskCreateWithCaps(speak_worker, "doubao_auto", TTS_TASK_STACK,
                                NULL, 4, NULL,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }
    size_t len = strlen(text);
    char *copy = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, text, len + 1);
    if (xQueueSend(s_async_queue, &copy, 0) != pdTRUE) {
        heap_caps_free(copy);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
