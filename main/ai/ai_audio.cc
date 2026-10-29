#include "ai_audio.h"
#include "ai_audio_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include "sdkconfig.h"
#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "es8311_codec.h"

#include "es8311_audio_codec.h"

extern "C" {
#include "hw.h"
#include "settings.h"
}

static const char *TAG = "ai.audio";

#define AI_SAMPLE_RATE 16000

static Es8311AudioCodec *s_codec;
static int s_volume = 70;
static bool s_muted;
static std::function<bool(std::vector<int16_t> &&)> s_play_pcm;
static std::function<bool(const uint8_t *, size_t)> s_play_ogg;
static std::function<bool(size_t)> s_input_open;
static std::function<size_t(int16_t *, size_t, uint32_t)> s_input_read;
static std::function<void()> s_input_close;
static std::function<bool()> s_playback_idle;
static std::function<void()> s_playback_stop;

static int clamp_volume(int v)
{
    if (v < 0) return 0;
    if (v > 100) return 100;
    return v;
}

esp_err_t ai_audio_init(void)
{
    if (s_codec != nullptr) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == nullptr) {
        esp_err_t err = bsp_i2c_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "i2c init failed: %s", esp_err_to_name(err));
            return err;
        }
        bus = bsp_i2c_get_handle();
    }
    if (bus == nullptr) {
        ESP_LOGE(TAG, "no i2c bus");
        return ESP_FAIL;
    }

    s_codec = new Es8311AudioCodec(
        bus, (i2c_port_t)BSP_I2C_NUM, AI_SAMPLE_RATE, AI_SAMPLE_RATE,
        (gpio_num_t)PIN_I2S_MCLK, (gpio_num_t)PIN_I2S_BCLK,
        (gpio_num_t)PIN_I2S_WS, (gpio_num_t)PIN_I2S_DOUT,
        (gpio_num_t)PIN_I2S_DIN, (gpio_num_t)PIN_PA_CTRL,
        ES8311_CODEC_DEFAULT_ADDR, true, false);
    if (s_codec == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    s_volume = clamp_volume(settings_get()->volume);
    s_codec->SetInputGain((float)CONFIG_TEMPLATE_XIAOZHI_MIC_GAIN_DB);
    s_codec->SetOutputVolume(s_volume);

    ESP_LOGI(TAG, "ES8311 16 kHz duplex ready, volume=%d gain=%d dB",
             s_volume, CONFIG_TEMPLATE_XIAOZHI_MIC_GAIN_DB);
    return ESP_OK;
}

Es8311AudioCodec *ai_audio_codec(void)
{
    return s_codec;
}

void ai_audio_set_play_pcm_callback(std::function<bool(std::vector<int16_t> &&)> callback)
{
    s_play_pcm = std::move(callback);
}

void ai_audio_set_play_ogg_callback(std::function<bool(const uint8_t *, size_t)> callback)
{
    s_play_ogg = std::move(callback);
}

void ai_audio_set_input_callbacks(
    std::function<bool(size_t)> open_cb,
    std::function<size_t(int16_t *, size_t, uint32_t)> read_cb,
    std::function<void()> close_cb)
{
    s_input_open = std::move(open_cb);
    s_input_read = std::move(read_cb);
    s_input_close = std::move(close_cb);
}

void ai_audio_set_playback_callbacks(std::function<bool()> idle_cb,
                                     std::function<void()> stop_cb)
{
    s_playback_idle = std::move(idle_cb);
    s_playback_stop = std::move(stop_cb);
}

bool ai_audio_input_open(size_t max_samples)
{
    return s_input_open && s_input_open(max_samples);
}

size_t ai_audio_read_pcm(int16_t *pcm, size_t samples, uint32_t timeout_ms)
{
    return s_input_read ? s_input_read(pcm, samples, timeout_ms) : 0;
}

void ai_audio_input_close(void)
{
    if (s_input_close) {
        s_input_close();
    }
}

bool ai_audio_playback_idle(void)
{
    return s_playback_idle ? s_playback_idle() : true;
}

void ai_audio_stop_playback(void)
{
    if (s_playback_stop) {
        s_playback_stop();
    }
}

bool ai_audio_available(void)
{
    return s_play_ogg != nullptr;
}

void ai_audio_output_format(int *sample_rate, int *channels, int *bits)
{
    if (sample_rate) *sample_rate = AI_SAMPLE_RATE;
    if (channels) *channels = 1;
    if (bits) *bits = 16;
}

bool ai_audio_write_pcm(const int16_t *pcm, size_t samples, uint32_t timeout_ms)
{
    if (s_play_pcm == nullptr || pcm == nullptr || samples == 0) {
        return false;
    }

    /* 60 ms chunks keep the AudioService playback queue (4 tasks) fed without
     * blocking the Lua UI task for a whole buffer. */
    const size_t chunk_samples = (size_t)AI_SAMPLE_RATE * 60 / 1000;
    int64_t deadline_us = timeout_ms ? esp_timer_get_time() + (int64_t)timeout_ms * 1000 : 0;

    size_t offset = 0;
    while (offset < samples) {
        size_t count = std::min(chunk_samples, samples - offset);
        std::vector<int16_t> chunk(pcm + offset, pcm + offset + count);

        while (!s_play_pcm(std::move(chunk))) {
            if (deadline_us != 0 && esp_timer_get_time() >= deadline_us) {
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            chunk.assign(pcm + offset, pcm + offset + count);
        }
        offset += count;
    }
    return true;
}

bool ai_audio_play_ogg_file(const char *path)
{
    if (s_play_ogg == nullptr || path == nullptr) {
        return false;
    }

    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        ESP_LOGW(TAG, "sound missing: %s", path);
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long size = ftell(file);
    rewind(file);
    /* Effects are a few KiB; reject anything that looks like a media file. */
    if (size <= 0 || size > 512 * 1024) {
        fclose(file);
        return false;
    }

    uint8_t *buf = (uint8_t *)heap_caps_malloc((size_t)size,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == nullptr) {
        fclose(file);
        return false;
    }
    size_t got = fread(buf, 1, (size_t)size, file);
    fclose(file);

    bool ok = got == (size_t)size && s_play_ogg(buf, (size_t)size);
    heap_caps_free(buf);
    if (!ok) {
        ESP_LOGD(TAG, "sound skipped: %s", path);
    }
    return ok;
}

void ai_audio_set_volume(int percent)
{
    s_volume = clamp_volume(percent);
    if (s_codec != nullptr) {
        s_codec->SetOutputVolume(s_volume);
    }
}

int ai_audio_get_volume(void)
{
    return s_volume;
}

void ai_audio_set_mute(bool muted)
{
    s_muted = muted;
}

bool ai_audio_is_muted(void)
{
    return s_muted;
}

static void play_tones(const float *freqs, const int *durations, int count, int amplitude)
{
    if (!s_play_pcm) return;

    size_t total = 0;
    for (int i = 0; i < count; i++) {
        total += (size_t)AI_SAMPLE_RATE * durations[i] / 1000;
    }
    std::vector<int16_t> pcm(total);
    size_t pos = 0;
    for (int i = 0; i < count; i++) {
        int frames = AI_SAMPLE_RATE * durations[i] / 1000;
        int fade = AI_SAMPLE_RATE / 100;
        for (int n = 0; n < frames; n++) {
            float env = 1.0f;
            if (n < fade) env = (float)n / fade;
            else if (n > frames - fade) env = (float)(frames - n) / fade;
            pcm[pos++] = (int16_t)(amplitude * env *
                sinf(2.0f * (float)M_PI * freqs[i] * n / AI_SAMPLE_RATE));
        }
    }
    s_play_pcm(std::move(pcm));
}

void ai_audio_beep(void)
{
    static const float freqs[] = {1000.0f};
    static const int durations[] = {200};
    play_tones(freqs, durations, 1, 8000);
}

void ai_audio_chime(void)
{
    static const float freqs[] = {880.0f, 1320.0f};
    static const int durations[] = {100, 120};
    play_tones(freqs, durations, 2, 7000);
}
