/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_image_ppa.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/lock.h>

#include "soc/soc_caps.h"

#if SOC_PPA_SUPPORTED
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#define LUA_IMAGE_PPA_ALIGNMENT 64U
static const char *TAG = "image_ppa";

static _lock_t s_ppa_lock;
static ppa_client_handle_t s_ppa_client;
static bool s_ppa_disabled;

static esp_err_t lua_image_ppa_init_locked(void)
{
    if (s_ppa_client != NULL) {
        return ESP_OK;
    }
    if (s_ppa_disabled) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const ppa_client_config_t config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    esp_err_t err = ppa_register_client(&config, &s_ppa_client);
    if (err != ESP_OK) {
        s_ppa_disabled = true;
        ESP_LOGE(TAG, "PPA client init failed: %s", esp_err_to_name(err));
    }
    return err;
}

static bool lua_image_ppa_input_config(lua_image_format_t format, ppa_srm_color_mode_t *color_mode, bool *rgb_swap, bool *byte_swap,
                                       size_t *bytes_per_pixel)
{
    *rgb_swap = false;
    *byte_swap = false;
    switch (format) {
    case LUA_IMAGE_FORMAT_RGB565LE:
        *color_mode = PPA_SRM_COLOR_MODE_RGB565;
        *bytes_per_pixel = 2;
        return true;
    case LUA_IMAGE_FORMAT_RGB565BE:
        *color_mode = PPA_SRM_COLOR_MODE_RGB565;
        *byte_swap = true;
        *bytes_per_pixel = 2;
        return true;
    case LUA_IMAGE_FORMAT_RGB888:
        *color_mode = PPA_SRM_COLOR_MODE_RGB888;
        *rgb_swap = true;
        *bytes_per_pixel = 3;
        return true;
    case LUA_IMAGE_FORMAT_BGR888:
        *color_mode = PPA_SRM_COLOR_MODE_RGB888;
        *bytes_per_pixel = 3;
        return true;
    case LUA_IMAGE_FORMAT_GRAY8:
        *color_mode = PPA_SRM_COLOR_MODE_GRAY8;
        *bytes_per_pixel = 1;
        return true;
    case LUA_IMAGE_FORMAT_YUYV:
        *color_mode = PPA_SRM_COLOR_MODE_YUV422_YUYV;
        *bytes_per_pixel = 2;
        return true;
    case LUA_IMAGE_FORMAT_UYVY:
        *color_mode = PPA_SRM_COLOR_MODE_YUV422_UYVY;
        *bytes_per_pixel = 2;
        return true;
    default:
        return false;
    }
}

static bool lua_image_ppa_output_config(lua_image_format_t format, ppa_srm_color_mode_t *color_mode, size_t *bytes_per_pixel)
{
    switch (format) {
    case LUA_IMAGE_FORMAT_RGB565LE:
        *color_mode = PPA_SRM_COLOR_MODE_RGB565;
        *bytes_per_pixel = 2;
        return true;
    case LUA_IMAGE_FORMAT_BGR888:
        *color_mode = PPA_SRM_COLOR_MODE_RGB888;
        *bytes_per_pixel = 3;
        return true;
    case LUA_IMAGE_FORMAT_GRAY8:
        *color_mode = PPA_SRM_COLOR_MODE_GRAY8;
        *bytes_per_pixel = 1;
        return true;
    default:
        return false;
    }
}

static bool lua_image_ppa_scale(int source, int target, uint16_t *scale_q4)
{
    uint64_t scaled_target = (uint64_t)(uint32_t)target * 16U;
    uint64_t scale = scaled_target / (uint32_t)source;
    if (scaled_target % (uint32_t)source != 0 || scale == 0 || scale >= 4096U) {
        return false;
    }
    *scale_q4 = (uint16_t)scale;
    return true;
}

typedef struct {
    int block_x;
    int block_y;
    int block_w;
    int block_h;
    int out_w;
    int out_h;
    int rotation_ccw;
    bool mirror_x;
    bool mirror_y;
    bool scaled;
    uint16_t scale_x_q4;
    uint16_t scale_y_q4;
} lua_image_ppa_job_t;

static esp_err_t lua_image_ppa_run(const lua_image_source_t *src, lua_image_format_t dst_format,
                                   const lua_image_ppa_job_t *job, lua_image_view_t *out)
{
    ppa_srm_color_mode_t in_mode;
    ppa_srm_color_mode_t out_mode;
    size_t in_bpp;
    size_t out_bpp;
    bool rgb_swap;
    bool byte_swap;
    size_t input_bytes;
    size_t output_bytes;
    size_t allocated_bytes;
    uint8_t *output = NULL;
    ppa_srm_rotation_angle_t rotation;

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (src == NULL || src->data == NULL || job == NULL || src->width <= 0 || src->height <= 0 ||
        job->out_w <= 0 || job->out_h <= 0 || job->block_w <= 0 || job->block_h <= 0 ||
        job->block_x < 0 || job->block_y < 0 ||
        job->block_x > src->width - job->block_w || job->block_y > src->height - job->block_h ||
        !lua_image_ppa_input_config(src->format, &in_mode, &rgb_swap, &byte_swap, &in_bpp) ||
        !lua_image_ppa_output_config(dst_format, &out_mode, &out_bpp)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* RGB-to-gray coefficients are global PPA state, so only gray pass-through is safe here. */
    if (dst_format == LUA_IMAGE_FORMAT_GRAY8 && src->format != LUA_IMAGE_FORMAT_GRAY8) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if ((src->format == LUA_IMAGE_FORMAT_UYVY || src->format == LUA_IMAGE_FORMAT_YUYV) && (src->width & 1) != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    switch (job->rotation_ccw) {
    case 0:
        rotation = PPA_SRM_ROTATION_ANGLE_0;
        break;
    case 90:
        rotation = PPA_SRM_ROTATION_ANGLE_90;
        break;
    case 180:
        rotation = PPA_SRM_ROTATION_ANGLE_180;
        break;
    case 270:
        rotation = PPA_SRM_ROTATION_ANGLE_270;
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
    if ((size_t)src->width > SIZE_MAX / (size_t)src->height / in_bpp ||
        (size_t)job->out_w > SIZE_MAX / (size_t)job->out_h / out_bpp) {
        return ESP_ERR_INVALID_SIZE;
    }
    input_bytes = (size_t)src->width * (size_t)src->height * in_bpp;
    output_bytes = (size_t)job->out_w * (size_t)job->out_h * out_bpp;
    if (input_bytes > UINT32_MAX || output_bytes > UINT32_MAX - (LUA_IMAGE_PPA_ALIGNMENT - 1) ||
        src->bytes < input_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }
    allocated_bytes = (output_bytes + LUA_IMAGE_PPA_ALIGNMENT - 1) & ~(LUA_IMAGE_PPA_ALIGNMENT - 1);

    _lock_acquire(&s_ppa_lock);
    esp_err_t err = lua_image_ppa_init_locked();
    if (err == ESP_OK) {
        output = (uint8_t *)heap_caps_aligned_alloc(LUA_IMAGE_PPA_ALIGNMENT, allocated_bytes,
                                                    MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        err = output != NULL ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        const ppa_srm_oper_config_t config = {
            .in = {
                .buffer = src->data,
                .pic_w = (uint32_t)src->width,
                .pic_h = (uint32_t)src->height,
                .block_w = (uint32_t)job->block_w,
                .block_h = (uint32_t)job->block_h,
                .block_offset_x = (uint32_t)job->block_x,
                .block_offset_y = (uint32_t)job->block_y,
                .srm_cm = in_mode,
                .yuv_range = PPA_COLOR_RANGE_LIMIT,
                .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
            },
            .out = {
                .buffer = output,
                .buffer_size = (uint32_t)allocated_bytes,
                .pic_w = (uint32_t)job->out_w,
                .pic_h = (uint32_t)job->out_h,
                .srm_cm = out_mode,
            },
            .rotation_angle = rotation,
            .scale_x = job->scaled ? (float)job->scale_x_q4 / 16.0f : 1.0f,
            .scale_y = job->scaled ? (float)job->scale_y_q4 / 16.0f : 1.0f,
            .mirror_x = job->mirror_x,
            .mirror_y = job->mirror_y,
            .rgb_swap = rgb_swap,
            .byte_swap = byte_swap,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        err = ppa_do_scale_rotate_mirror(s_ppa_client, &config);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PPA transform failed, using CPU: %s", esp_err_to_name(err));
        free(output);
    } else {
        out->data = output;
        out->bytes = output_bytes;
        out->width = job->out_w;
        out->height = job->out_h;
        out->format = dst_format;
        out->owned = true;
        strlcpy(out->source_format, src->source_format, sizeof(out->source_format));
    }
    _lock_release(&s_ppa_lock);
    return err;
}

esp_err_t lua_image_ppa_transform(const lua_image_source_t *src, lua_image_format_t dst_format, int dst_width, int dst_height,
                                  lua_image_view_t *out)
{
    uint16_t scale_x;
    uint16_t scale_y;
    lua_image_ppa_job_t job;

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (src == NULL || src->data == NULL || src->width <= 0 || src->height <= 0 ||
        dst_width <= 0 || dst_height <= 0 ||
        !lua_image_ppa_scale(src->width, dst_width, &scale_x) ||
        !lua_image_ppa_scale(src->height, dst_height, &scale_y)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    job.block_x = 0;
    job.block_y = 0;
    job.block_w = src->width;
    job.block_h = src->height;
    job.out_w = dst_width;
    job.out_h = dst_height;
    job.rotation_ccw = 0;
    job.mirror_x = false;
    job.mirror_y = false;
    job.scaled = true;
    job.scale_x_q4 = scale_x;
    job.scale_y_q4 = scale_y;
    return lua_image_ppa_run(src, dst_format, &job, out);
}

esp_err_t lua_image_ppa_transform_ex(const lua_image_source_t *src, lua_image_format_t dst_format,
                                     int dst_width, int dst_height, const lua_image_ppa_op_t *op,
                                     lua_image_view_t *out)
{
    lua_image_ppa_job_t job;
    int expect_w;
    int expect_h;

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (src == NULL || src->data == NULL || src->width <= 0 || src->height <= 0 || dst_width <= 0 || dst_height <= 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    job.block_x = op ? op->crop_x : 0;
    job.block_y = op ? op->crop_y : 0;
    job.block_w = (op && op->crop_w > 0) ? op->crop_w : src->width;
    job.block_h = (op && op->crop_h > 0) ? op->crop_h : src->height;
    job.rotation_ccw = op ? op->rotation_ccw : 0;
    job.mirror_x = op ? op->mirror_x : false;
    job.mirror_y = op ? op->mirror_y : false;
    job.scaled = false;
    job.scale_x_q4 = 16;
    job.scale_y_q4 = 16;

    expect_w = (job.rotation_ccw == 90 || job.rotation_ccw == 270) ? job.block_h : job.block_w;
    expect_h = (job.rotation_ccw == 90 || job.rotation_ccw == 270) ? job.block_w : job.block_h;
    if (dst_width != expect_w || dst_height != expect_h) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    job.out_w = dst_width;
    job.out_h = dst_height;
    return lua_image_ppa_run(src, dst_format, &job, out);
}
#else
esp_err_t lua_image_ppa_transform(const lua_image_source_t *src, lua_image_format_t dst_format, int dst_width, int dst_height,
                                  lua_image_view_t *out)
{
    (void)src;
    (void)dst_format;
    (void)dst_width;
    (void)dst_height;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t lua_image_ppa_transform_ex(const lua_image_source_t *src, lua_image_format_t dst_format,
                                     int dst_width, int dst_height, const lua_image_ppa_op_t *op,
                                     lua_image_view_t *out)
{
    (void)src;
    (void)dst_format;
    (void)dst_width;
    (void)dst_height;
    (void)op;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    return ESP_ERR_NOT_SUPPORTED;
}
#endif
