/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "lua_image_convert.h"

esp_err_t lua_image_ppa_transform(const lua_image_source_t *src, lua_image_format_t dst_format, int dst_width, int dst_height,
                                  lua_image_view_t *out);

/**
 * @brief Optional crop/rotate/mirror applied during a PPA transform.
 *
 * crop_w/crop_h <= 0 select the full source picture. rotation_ccw is the PPA
 * direction (counter-clockwise). The destination size is implied by the crop
 * rectangle and rotation; callers that need scaling use lua_image_ppa_transform().
 */
typedef struct {
    int crop_x;
    int crop_y;
    int crop_w;
    int crop_h;
    int rotation_ccw;
    bool mirror_x;
    bool mirror_y;
} lua_image_ppa_op_t;

esp_err_t lua_image_ppa_transform_ex(const lua_image_source_t *src, lua_image_format_t dst_format,
                                     int dst_width, int dst_height, const lua_image_ppa_op_t *op,
                                     lua_image_view_t *out);
