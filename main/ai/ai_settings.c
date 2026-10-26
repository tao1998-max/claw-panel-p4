#include "ai_settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

static const char *TAG = "ai.settings";

#define AI_NS        "ai"
#define KEY_EN       "en"
#define KEY_WAKE     "wake"
#define KEY_MUTE     "mute"
#define KEY_OTA      "ota_url"
#define KEY_WS       "ws_url"
#define KEY_TOKEN    "token"

#define AI_URL_MAX   192
#define AI_TOKEN_MAX 192

#ifndef CONFIG_TEMPLATE_XIAOZHI_OTA_URL
#define CONFIG_TEMPLATE_XIAOZHI_OTA_URL "https://api.tenclass.net/xiaozhi/ota/"
#endif

static bool s_inited;
static bool s_enabled = true;
static bool s_wake = true;
static bool s_mute;
static char s_ota_url[AI_URL_MAX] = CONFIG_TEMPLATE_XIAOZHI_OTA_URL;
static char s_ws_url[AI_URL_MAX];
static char s_token[AI_TOKEN_MAX];

static esp_err_t read_str(nvs_handle_t h, const char *key, char *out, size_t out_size)
{
    size_t len = out_size;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    if (err != ESP_OK) {
        out[0] = '\0';
    }
    return err;
}

static esp_err_t write_str(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(AI_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_str(h, key, value != NULL ? value : "");
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void write_bool(const char *key, bool value)
{
    nvs_handle_t h;
    if (nvs_open(AI_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_u8(h, key, value ? 1 : 0) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

static bool read_bool(nvs_handle_t h, const char *key, bool def)
{
    uint8_t v = def ? 1 : 0;
    if (nvs_get_u8(h, key, &v) != ESP_OK) {
        return def;
    }
    return v != 0;
}

esp_err_t ai_settings_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    s_inited = true;

    nvs_handle_t h;
    esp_err_t err = nvs_open(AI_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no ai settings, defaults");
        return err;
    }

    s_enabled = read_bool(h, KEY_EN, true);
    s_wake = read_bool(h, KEY_WAKE, true);
    s_mute = read_bool(h, KEY_MUTE, false);

    char tmp[AI_URL_MAX];
    if (read_str(h, KEY_OTA, tmp, sizeof(tmp)) == ESP_OK && tmp[0] != '\0') {
        strlcpy(s_ota_url, tmp, sizeof(s_ota_url));
    }
    if (read_str(h, KEY_WS, tmp, sizeof(tmp)) == ESP_OK) {
        strlcpy(s_ws_url, tmp, sizeof(s_ws_url));
    }
    if (read_str(h, KEY_TOKEN, tmp, sizeof(tmp)) == ESP_OK) {
        strlcpy(s_token, tmp, sizeof(s_token));
    }
    nvs_close(h);

    ESP_LOGI(TAG, "enabled=%d wake=%d mute=%d ws=%s", s_enabled, s_wake, s_mute,
             s_ws_url[0] ? s_ws_url : "(ota)");
    return ESP_OK;
}

bool ai_settings_enabled(void) { return s_enabled; }
void ai_settings_set_enabled(bool enabled)
{
    s_enabled = enabled;
    write_bool(KEY_EN, enabled);
}

bool ai_settings_wake_word(void) { return s_wake; }
void ai_settings_set_wake_word(bool enabled)
{
    s_wake = enabled;
    write_bool(KEY_WAKE, enabled);
}

bool ai_settings_muted(void) { return s_mute; }
void ai_settings_set_muted(bool muted)
{
    s_mute = muted;
    write_bool(KEY_MUTE, muted);
}

const char *ai_settings_ota_url(void) { return s_ota_url; }
esp_err_t ai_settings_set_ota_url(const char *url)
{
    if (url == NULL || (strncmp(url, "http://", 7) != 0 &&
                        strncmp(url, "https://", 8) != 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_ota_url, url, sizeof(s_ota_url));
    ai_settings_clear_activation();
    return write_str(KEY_OTA, s_ota_url);
}

const char *ai_settings_ws_url(void) { return s_ws_url; }
const char *ai_settings_token(void) { return s_token; }
esp_err_t ai_settings_set_direct_ws(const char *url, const char *token)
{
    if (url != NULL && url[0] != '\0' &&
        strncmp(url, "ws://", 5) != 0 && strncmp(url, "wss://", 6) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_ws_url, url != NULL ? url : "", sizeof(s_ws_url));
    strlcpy(s_token, token != NULL ? token : "", sizeof(s_token));
    ai_settings_clear_activation();
    esp_err_t err = write_str(KEY_WS, s_ws_url);
    if (err == ESP_OK) {
        err = write_str(KEY_TOKEN, s_token);
    }
    return err;
}

esp_err_t ai_settings_clear_activation(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("websocket", NVS_READWRITE, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
