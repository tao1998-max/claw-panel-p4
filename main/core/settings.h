#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================
 *  系统参数：NVS 持久化 + 硬件副作用
 *
 *  分层约定：
 *   - ui 层只调这里的 setter，不直接碰 hw_*（除了只读状态）
 *   - settings_init() 只负责「读出来」，不碰硬件，因为显示/音频可能还没初始化
 *   - 开机流程里要在 hw_display_init() + hw_audio_init() 之后调一次 settings_apply()
 * ========================================================================== */

typedef struct {
    uint8_t  brightness;    /* 10-100 */
    uint8_t  volume;        /* 0-100 */
    uint8_t  lang;          /* i18n_lang_t */
    uint16_t screen_off_s;  /* 0=常亮，或 30/60/300 秒 */
} sys_settings_t;

/* 从 NVS 读取（缺省值兜底）。不写硬件、不重绘 UI。 */
esp_err_t settings_init(void);

/* 把当前参数推到硬件（亮度/音量/语言）。开机时在显示与音频初始化之后调用。 */
void settings_apply(void);

const sys_settings_t *settings_get(void);

void settings_set_brightness(int percent);   /* 立即生效 + 存盘 */
void settings_set_volume(int percent);

/* 拖动滑块时用：只改硬件不改 NVS，松手时再调上面的 set_* 落盘 */
void settings_preview_brightness(int percent);
void settings_preview_volume(int percent);

/* 只存盘 + 切语言；界面重绘由调用方决定（UI 线程里要 ui_rebuild()） */
void settings_set_lang(int lang);

void settings_set_screen_off(int seconds);

/* 擦除 NVS 并重启 */
void settings_factory_reset(void);

#ifdef __cplusplus
}
#endif
