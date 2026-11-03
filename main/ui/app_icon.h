#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Decode an App icon (baseline JPEG, 96x96 recommended) from an absolute FAT
 * or SD path into an RGB565 LVGL image descriptor. Returns NULL when the file
 * is missing or cannot be decoded. Free with app_icon_free(). */
lv_image_dsc_t *app_icon_load(const char *path);
void app_icon_free(lv_image_dsc_t *dsc);

#ifdef __cplusplus
}
#endif
