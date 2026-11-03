/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_image_transform.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "image_transform";

static size_t lua_image_bpp(lua_image_format_t format)
{
    if (format == LUA_IMAGE_FORMAT_RGB565LE) {
        return 2;
    }
    if (format == LUA_IMAGE_FORMAT_GRAY8) {
        return 1;
    }
    return 0;
}

static void lua_image_transform_cpu(const uint8_t *src, int sw, int sh,
                                    int crop_x, int crop_y, int crop_w, int crop_h,
                                    uint8_t *dst, int dst_w, int dst_h,
                                    size_t bpp, int rotation_cw, bool mirror_x, bool mirror_y)
{
    for (int y = 0; y < dst_h; y++) {
        for (int x = 0; x < dst_w; x++) {
            int ox = mirror_x ? dst_w - 1 - x : x;
            int oy = mirror_y ? dst_h - 1 - y : y;
            int src_x;
            int src_y;

            switch (rotation_cw) {
            case 90:
                src_x = oy;
                src_y = crop_h - 1 - ox;
                break;
            case 180:
                src_x = crop_w - 1 - ox;
                src_y = crop_h - 1 - oy;
                break;
            case 270:
                src_x = crop_w - 1 - oy;
                src_y = ox;
                break;
            default:
                src_x = ox;
                src_y = oy;
                break;
            }
            const uint8_t *sp = src + ((size_t)(crop_y + src_y) * (size_t)sw + (size_t)(crop_x + src_x)) * bpp;
            uint8_t *dp = dst + ((size_t)y * (size_t)dst_w + (size_t)x) * bpp;
            if (bpp == 1) {
                dp[0] = sp[0];
            } else {
                dp[0] = sp[0];
                dp[1] = sp[1];
            }
        }
    }
}

esp_err_t lua_image_transform_view(const lua_image_source_t *src, lua_image_format_t dst_format,
                                   const lua_image_op_t *op, lua_image_view_t *out)
{
    lua_image_ppa_op_t ppa_op = {0};
    lua_image_view_t view = {0};
    lua_image_op_t req = {0};
    int rotation_cw;
    int crop_w;
    int crop_h;
    int dst_w;
    int dst_h;
    size_t bpp;
    size_t bytes;
    uint8_t *pixels;
    esp_err_t err;

    if (src == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (src->data == NULL || src->bytes == 0 || src->width <= 0 || src->height <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (op != NULL) {
        req = *op;
    }
    rotation_cw = req.rotation_cw;
    if (rotation_cw != 0 && rotation_cw != 90 && rotation_cw != 180 && rotation_cw != 270) {
        return ESP_ERR_INVALID_ARG;
    }
    crop_w = req.crop_w > 0 ? req.crop_w : src->width;
    crop_h = req.crop_h > 0 ? req.crop_h : src->height;
    if (req.crop_x < 0) {
        req.crop_x = 0;
    }
    if (req.crop_y < 0) {
        req.crop_y = 0;
    }
    if (req.crop_x >= src->width || req.crop_y >= src->height ||
        crop_w <= 0 || crop_h <= 0 ||
        crop_w > src->width - req.crop_x || crop_h > src->height - req.crop_y) {
        return ESP_ERR_INVALID_SIZE;
    }
    dst_w = (rotation_cw == 90 || rotation_cw == 270) ? crop_h : crop_w;
    dst_h = (rotation_cw == 90 || rotation_cw == 270) ? crop_w : crop_h;

    ppa_op.crop_x = req.crop_x;
    ppa_op.crop_y = req.crop_y;
    ppa_op.crop_w = crop_w;
    ppa_op.crop_h = crop_h;
    ppa_op.rotation_ccw = (360 - rotation_cw) % 360;
    ppa_op.mirror_x = req.mirror_x;
    ppa_op.mirror_y = req.mirror_y;
    err = lua_image_ppa_transform_ex(src, dst_format, dst_w, dst_h, &ppa_op, &view);
    if (err == ESP_OK) {
        *out = view;
        return ESP_OK;
    }
    if (err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "PPA transform failed: %s", esp_err_to_name(err));
    }

    /* CPU fallback only handles buffers that are already in the output format. */
    bpp = lua_image_bpp(dst_format);
    if (bpp == 0 || src->format != dst_format) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if ((size_t)dst_w > SIZE_MAX / (size_t)dst_h / bpp) {
        return ESP_ERR_INVALID_SIZE;
    }
    bytes = (size_t)dst_w * (size_t)dst_h * bpp;
    pixels = (uint8_t *)heap_caps_aligned_calloc(16, 1, bytes, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (pixels == NULL) {
        ESP_LOGE(TAG, "transform buffer alloc failed: %u bytes", (unsigned)bytes);
        return ESP_ERR_NO_MEM;
    }
    lua_image_transform_cpu(src->data, src->width, src->height,
                            req.crop_x, req.crop_y, crop_w, crop_h,
                            pixels, dst_w, dst_h, bpp, rotation_cw, req.mirror_x, req.mirror_y);

    out->data = pixels;
    out->bytes = bytes;
    out->width = dst_w;
    out->height = dst_h;
    out->format = dst_format;
    out->owned = true;
    strlcpy(out->source_format, src->source_format, sizeof(out->source_format));
    return ESP_OK;
}
