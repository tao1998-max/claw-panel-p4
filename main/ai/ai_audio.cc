#include "ai_audio.h"
#include "ai_audio_internal.h"

#include <cmath>
#include <cstring>

#include "sdkconfig.h"
#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
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
