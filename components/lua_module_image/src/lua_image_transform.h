/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lua_image_convert.h"
#include "lua_image_ppa.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Crop / rotate / mirror request for one image frame.
 *
 * Crop dimensions <= 0 select the full source. Rotation is clockwise
 * (0/90/180/270) and swaps the output dimensions at 90/270. Mirrors are
 * applied after rotation.
 */
typedef struct {
    int crop_x;
    int crop_y;
    int crop_w;
    int crop_h;
    int rotation_cw;
    bool mirror_x;
    bool mirror_y;
} lua_image_op_t;

/**
 * @brief Run crop/rotate/mirror through PPA, falling back to CPU for
 *        same-format RGB565LE/GRAY8 buffers.
 *
 * The returned view is owned and must be released with
 * lua_image_release_view(). Returns ESP_ERR_NOT_SUPPORTED when neither path
 * can handle the source format.
 */
esp_err_t lua_image_transform_view(const lua_image_source_t *src, lua_image_format_t dst_format,
                                   const lua_image_op_t *op, lua_image_view_t *out);

#ifdef __cplusplus
}
#endif
