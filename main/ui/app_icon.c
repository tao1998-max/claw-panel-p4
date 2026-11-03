/*
 * App icon decoder: baseline JPEG (any size up to 256x256) -> LVGL RGB565.
 *
 * Icons live inside the App package (FAT or SD), so this decodes straight from
 * an absolute VFS path; it never touches the LVGL filesystem driver, which is
 * rooted at the FAT workspace only.
 */
#include "app_icon.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_jpeg_common.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"

#define ICON_MAX_FILE_BYTES (256 * 1024)
#define ICON_MAX_DIM        256

static const char *TAG = "ui.icon";

lv_image_dsc_t *app_icon_load(const char *path)
{
    if (!path || !path[0]) {
        return NULL;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        ESP_LOGD(TAG, "icon missing: %s", path);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size <= 0 || size > ICON_MAX_FILE_BYTES || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        ESP_LOGW(TAG, "icon size rejected: %s (%ld)", path, size);
        return NULL;
    }
    uint8_t *input = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!input) {
        input = malloc((size_t)size);
    }
    if (!input) {
        fclose(file);
        return NULL;
    }
    size_t read_bytes = fread(input, 1, (size_t)size, file);
    fclose(file);
    if (read_bytes != (size_t)size) {
        free(input);
        return NULL;
    }

    jpeg_dec_config_t config = {
        .output_type = JPEG_PIXEL_FORMAT_RGB565_LE,
        .scale = { .width = 0, .height = 0 },
    };
    jpeg_dec_handle_t decoder = NULL;
    if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK || !decoder) {
        free(input);
        return NULL;
    }

    jpeg_dec_io_t io = {0};
    io.inbuf = input;
    io.inbuf_len = (int)size;
    jpeg_dec_header_info_t info = {0};
    lv_image_dsc_t *dsc = NULL;
    uint8_t *output = NULL;

    if (jpeg_dec_parse_header(decoder, &io, &info) != JPEG_ERR_OK ||
        info.width == 0 || info.height == 0 ||
        info.width > ICON_MAX_DIM || info.height > ICON_MAX_DIM) {
        ESP_LOGW(TAG, "icon header rejected: %s", path);
        goto cleanup;
    }

    int out_len = 0;
    if (jpeg_dec_get_outbuf_len(decoder, &out_len) != JPEG_ERR_OK ||
        out_len <= 0 || (size_t)out_len != (size_t)info.width * info.height * 2) {
        ESP_LOGW(TAG, "unexpected icon format: %s", path);
        goto cleanup;
    }

    output = heap_caps_aligned_alloc(16, (size_t)out_len,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!output) {
        output = heap_caps_aligned_alloc(16, (size_t)out_len, MALLOC_CAP_8BIT);
    }
    if (!output) {
        goto cleanup;
    }
    io.outbuf = output;
    io.out_size = out_len;
    if (jpeg_dec_process(decoder, &io) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "icon decode failed: %s", path);
        goto cleanup;
    }

    dsc = heap_caps_calloc(1, sizeof(*dsc), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dsc) {
        dsc = calloc(1, sizeof(*dsc));
    }
    if (!dsc) {
        goto cleanup;
    }
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = info.width;
    dsc->header.h = info.height;
    dsc->header.stride = (uint32_t)info.width * 2;
    dsc->data_size = (uint32_t)out_len;
    dsc->data = output;

cleanup:
    jpeg_dec_close(decoder);
    free(input);
    if (!dsc) {
        heap_caps_free(output);
    }
    return dsc;
}

void app_icon_free(lv_image_dsc_t *dsc)
{
    if (!dsc) {
        return;
    }
    if (dsc->data) {
        heap_caps_free((void *)dsc->data);
    }
    heap_caps_free(dsc);
}
