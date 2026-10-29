#include "ai_assets.h"

#include <stdio.h>
#include <stdlib.h>

#include "claw_storage.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "ai.assets";

#define FONT_PATH  AI_ASSETS_DIR "/font_puhui_common_20_4.bin"
#define MODEL_PATH AI_ASSETS_DIR "/srmodels.bin"

static uint8_t *s_font;
static size_t s_font_size;
static uint8_t *s_model_data;
static size_t s_model_size;
static srmodel_list_t *s_models;
static bool s_inited;

static uint8_t *read_file(const char *path, size_t *out_size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        ESP_LOGW(TAG, "open %s failed", path);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size <= 0) {
        fclose(file);
        ESP_LOGW(TAG, "%s is empty", path);
        return NULL;
    }
    rewind(file);

    uint8_t *buf = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        fclose(file);
        ESP_LOGE(TAG, "no PSRAM for %s (%ld bytes)", path, size);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        ESP_LOGW(TAG, "short read %s: %u/%ld", path, (unsigned)got, size);
        heap_caps_free(buf);
        return NULL;
    }
    *out_size = (size_t)size;
    return buf;
}

esp_err_t ai_assets_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    /* The AI font/model live in the shared 4 MB FAT. Mount it before ui_init;
     * never format here (the Claw path owns the blank-partition format). */
    esp_err_t err = claw_storage_mount_fs(false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "shared FAT mount failed: %s (flash assets with "
                 "`idf.py claw-flash`)", esp_err_to_name(err));
        return err;
    }
    s_inited = true;
    ESP_LOGI(TAG, "AI assets root: %s", AI_ASSETS_DIR);

    s_font = read_file(FONT_PATH, &s_font_size);
    if (s_font != NULL) {
        ESP_LOGI(TAG, "chat font loaded: %u bytes", (unsigned)s_font_size);
    } else {
        ESP_LOGW(TAG, "chat font missing; run `idf.py claw-flash`");
    }

    s_model_data = read_file(MODEL_PATH, &s_model_size);
    if (s_model_data == NULL) {
        ESP_LOGW(TAG, "srmodels missing; run `idf.py claw-flash`");
    }
    if (s_model_data != NULL) {
        s_models = srmodel_load(s_model_data);
        if (s_models != NULL) {
            ESP_LOGI(TAG, "srmodels loaded, %d model(s)", s_models->num);
            for (int i = 0; i < s_models->num; i++) {
                ESP_LOGI(TAG, "  model[%d]=%s", i, s_models->model_name[i]);
            }
        } else {
            ESP_LOGE(TAG, "srmodel_load failed");
            heap_caps_free(s_model_data);
            s_model_data = NULL;
        }
    }

    return ESP_OK;
}

uint8_t *ai_assets_font_buffer(size_t *size)
{
    if (size != NULL) *size = s_font_size;
    return s_font;
}

srmodel_list_t *ai_assets_models(void)
{
    return s_models;
}
