#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "hw.h"
#include "sdkconfig.h"
#include "ai_audio.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hw.audio";

#define AUDIO_SAMPLE_RATE   44100
#define AUDIO_CHANNELS      2
#define BEEP_FREQ_HZ        1000
#define BEEP_MS             200

#if !CONFIG_TEMPLATE_ENABLE_XIAOZHI
static esp_codec_dev_handle_t s_spk;
static esp_codec_dev_handle_t s_mic;
#endif
static int s_volume = 70;

#if !CONFIG_TEMPLATE_ENABLE_XIAOZHI
static void beep_task(void *arg)
{
    const int frames = AUDIO_SAMPLE_RATE * BEEP_MS / 1000;
    const size_t bytes = (size_t)frames * AUDIO_CHANNELS * sizeof(int16_t);
    int16_t *buf = malloc(bytes);
    if (buf == NULL) {
        vTaskDelete(NULL);
        return;
    }

    for (int i = 0; i < frames; i++) {
        /* 头尾各 20ms 做淡入淡出，避免爆音 */
        float env = 1.0f;
        int fade = AUDIO_SAMPLE_RATE / 50;
        if (i < fade) {
            env = (float)i / fade;
        } else if (i > frames - fade) {
            env = (float)(frames - i) / fade;
        }
        int16_t v = (int16_t)(11000.0f * env *
                              sinf(2.0f * (float)M_PI * BEEP_FREQ_HZ * i / AUDIO_SAMPLE_RATE));
        buf[i * 2]     = v;
        buf[i * 2 + 1] = v;
    }

    esp_codec_dev_write(s_spk, buf, (int)bytes);
    free(buf);
    vTaskDelete(NULL);
}
#endif

esp_err_t hw_audio_init(void)
{
    hw_status_t *st = &g_hw[HW_AUDIO];
    st->enabled = true;

#if CONFIG_TEMPLATE_ENABLE_XIAOZHI
    /* 小智语音链路独占 ES8311/I2S，统一 16 kHz 单声道。
     * 绝不能再调用 bsp_audio_init()，否则 44.1 kHz 句柄会和它抢同一组 I2S。 */
    esp_err_t err = ai_audio_init();
    st->ok = (err == ESP_OK);
    snprintf(st->detail, sizeof(st->detail), "%s", st->ok ? "ES8311 16k AI" : "AI audio fail");
    ESP_LOGI(TAG, "AI audio delegated: %s", esp_err_to_name(err));
    return err;
#else
    esp_err_t err = bsp_audio_init(NULL);   /* 默认 44.1k / 16bit / 立体声 */
    if (err != ESP_OK) {
        st->ok = false;
        snprintf(st->detail, sizeof(st->detail), "i2s err %d", err);
        return err;
    }

    s_spk = bsp_audio_codec_speaker_init();
    s_mic = bsp_audio_codec_microphone_init();

    if (s_spk == NULL) {
        st->ok = false;
        snprintf(st->detail, sizeof(st->detail), "ES8311 not found");
        ESP_LOGE(TAG, "speaker codec init failed");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = AUDIO_CHANNELS,
        .sample_rate = AUDIO_SAMPLE_RATE,
    };
    esp_codec_dev_open(s_spk, &fs);
    esp_codec_dev_set_out_vol(s_spk, s_volume);

    if (s_mic != NULL) {
        esp_codec_dev_open(s_mic, &fs);
        esp_codec_dev_set_in_gain(s_mic, 30.0);
    }

    st->ok = true;
    snprintf(st->detail, sizeof(st->detail), "ES8311 %s", s_mic ? "spk+mic" : "spk only");
    ESP_LOGI(TAG, "audio ready, volume=%d", s_volume);
    return ESP_OK;
#endif
}

void hw_audio_set_volume(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_volume = percent;
#if CONFIG_TEMPLATE_ENABLE_XIAOZHI
    ai_audio_set_volume(percent);
#else
    if (s_spk != NULL) {
        esp_codec_dev_set_out_vol(s_spk, percent);
    }
#endif
}

int hw_audio_get_volume(void)
{
#if CONFIG_TEMPLATE_ENABLE_XIAOZHI
    return ai_audio_get_volume();
#else
    return s_volume;
#endif
}

void hw_audio_beep(void)
{
#if CONFIG_TEMPLATE_ENABLE_XIAOZHI
    ai_audio_beep();
#else
    if (s_spk == NULL) {
        return;
    }
    xTaskCreate(beep_task, "beep", 4096, NULL, 5, NULL);
#endif
}
