#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "hw.h"
#include "i18n.h"

static const char *TAG = "settings";

#define NVS_NAMESPACE   "sys"
#define KEY_BRIGHTNESS  "bright"
#define KEY_VOLUME      "vol"
#define KEY_LANG        "lang"
#define KEY_SCREEN_OFF  "soff"

#define DEF_BRIGHTNESS  80
#define DEF_VOLUME      70
#define DEF_LANG        I18N_LANG_ZH
#define DEF_SCREEN_OFF  300

static sys_settings_t s_cfg = {
    .brightness = DEF_BRIGHTNESS,
    .volume = DEF_VOLUME,
    .lang = DEF_LANG,
    .screen_off_s = DEF_SCREEN_OFF,
};

/* ---------------- 取值校验：NVS 里的脏数据不能让硬件跑到非法区间 ---------------- */

static uint8_t clamp_brightness(int v)
{
    if (v < 10) return 10;
    if (v > 100) return 100;
    return (uint8_t)v;
}

static uint8_t clamp_volume(int v)
{
    if (v < 0) return 0;
    if (v > 100) return 100;
    return (uint8_t)v;
}

static uint8_t clamp_lang(int v)
{
    return (v == I18N_LANG_EN) ? I18N_LANG_EN : I18N_LANG_ZH;
}

static uint16_t clamp_screen_off(int v)
{
    switch (v) {
    case 0:
    case 30:
    case 60:
    case 300:
        return (uint16_t)v;
    default:
        return DEF_SCREEN_OFF;
    }
}

/* ---------------- NVS 读写 ---------------- */

static esp_err_t nvs_open_cfg(nvs_open_mode_t mode, nvs_handle_t *out)
{
    return nvs_open(NVS_NAMESPACE, mode, out);
}

static uint8_t read_u8(nvs_handle_t h, const char *key, uint8_t def)
{
    uint8_t v = def;
    if (nvs_get_u8(h, key, &v) != ESP_OK) {
        return def;
    }
    return v;
}

static void write_u8(const char *key, uint8_t value)
{
    nvs_handle_t h;
    if (nvs_open_cfg(NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed, %s not saved", key);
        return;
    }
    esp_err_t err = nvs_set_u8(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save %s failed: %s", key, esp_err_to_name(err));
    }
}

/* 息屏时间是 300 秒这种 >255 的值，必须用 u16 存 */
static uint16_t read_u16(nvs_handle_t h, const char *key, uint16_t def)
{
    uint16_t v = def;
    if (nvs_get_u16(h, key, &v) != ESP_OK) {
        return def;
    }
    return v;
}

static void write_u16(const char *key, uint16_t value)
{
    nvs_handle_t h;
    if (nvs_open_cfg(NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed, %s not saved", key);
        return;
    }
    esp_err_t err = nvs_set_u16(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save %s failed: %s", key, esp_err_to_name(err));
    }
}

/* ---------------- 对外接口 ---------------- */

esp_err_t settings_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_cfg(NVS_READONLY, &h);
    if (err != ESP_OK) {
        /* 第一次上电 NVS 里还没有这个命名空间，用缺省值继续跑 */
        ESP_LOGW(TAG, "no saved settings (%s), use defaults", esp_err_to_name(err));
        i18n_set_lang((i18n_lang_t)s_cfg.lang);
        return err;
    }

    s_cfg.brightness = clamp_brightness(read_u8(h, KEY_BRIGHTNESS, DEF_BRIGHTNESS));
    s_cfg.volume = clamp_volume(read_u8(h, KEY_VOLUME, DEF_VOLUME));
    s_cfg.lang = clamp_lang(read_u8(h, KEY_LANG, DEF_LANG));
    s_cfg.screen_off_s = clamp_screen_off(read_u16(h, KEY_SCREEN_OFF, DEF_SCREEN_OFF));
    nvs_close(h);

    i18n_set_lang((i18n_lang_t)s_cfg.lang);

    ESP_LOGI(TAG, "loaded: brightness=%u volume=%u lang=%u screen_off=%us",
             s_cfg.brightness, s_cfg.volume, s_cfg.lang, s_cfg.screen_off_s);
    return ESP_OK;
}

void settings_apply(void)
{
    hw_display_set_brightness(s_cfg.brightness);
    hw_audio_set_volume(s_cfg.volume);
    i18n_set_lang((i18n_lang_t)s_cfg.lang);
    ESP_LOGI(TAG, "applied: brightness=%u volume=%u lang=%u",
             s_cfg.brightness, s_cfg.volume, s_cfg.lang);
}

const sys_settings_t *settings_get(void)
{
    return &s_cfg;
}

void settings_set_brightness(int percent)
{
    uint8_t v = clamp_brightness(percent);
    if (v == s_cfg.brightness) {
        return;
    }
    s_cfg.brightness = v;
    hw_display_set_brightness(v);
    write_u8(KEY_BRIGHTNESS, v);
}

void settings_set_volume(int percent)
{
    uint8_t v = clamp_volume(percent);
    if (v == s_cfg.volume) {
        return;
    }
    s_cfg.volume = v;
    hw_audio_set_volume(v);
    write_u8(KEY_VOLUME, v);
}

void settings_preview_brightness(int percent)
{
    hw_display_set_brightness(clamp_brightness(percent));
}

void settings_preview_volume(int percent)
{
    hw_audio_set_volume(clamp_volume(percent));
}

void settings_set_lang(int lang)
{
    uint8_t v = clamp_lang(lang);
    if (v == s_cfg.lang) {
        return;
    }
    s_cfg.lang = v;
    i18n_set_lang((i18n_lang_t)v);
    write_u8(KEY_LANG, v);
}

void settings_set_screen_off(int seconds)
{
    uint16_t v = clamp_screen_off(seconds);
    if (v == s_cfg.screen_off_s) {
        return;
    }
    s_cfg.screen_off_s = v;
    write_u16(KEY_SCREEN_OFF, v);
}

void settings_factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset requested, erasing NVS");
    nvs_flash_erase();
    esp_restart();
}
