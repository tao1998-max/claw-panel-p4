#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "model_path.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ai_assets_init(void);
uint8_t *ai_assets_font_buffer(size_t *size);
srmodel_list_t *ai_assets_models(void);

#ifdef __cplusplus
}
#endif
