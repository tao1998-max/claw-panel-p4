#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "hw.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

static const char *TAG = "hw.display";

static lv_display_t *s_disp;
static int s_brightness = 80;

esp_err_t hw_display_init(void)
{
    hw_status_t *st = &g_hw[HW_DISPLAY];
    st->enabled = true;

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = {
            .task_priority = 4,
            /* ThorVG's Lottie software rasterizer needs ~22 KB of stack per
             * rendered frame; keep the stack in PSRAM so internal RAM stays
             * available for Wi-Fi/audio. */
            .task_stack = 32 * 1024,
            .task_stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
            .task_affinity = -1,
            .task_max_sleep_ms = 500,
            .timer_period_ms = 5,
        },
        /* avoid-tearing 模式下 BSP 会忽略这里的尺寸，改用两块全屏 DPI
         * framebuffer（1024×600×2 ×2 ≈ 2.4MB PSRAM）作为 LVGL draw buffer。
         * 保留 60 行配置，便于关闭 avoid-tearing 时回退到部分刷新。 */
        .buffer_size = BSP_LCD_H_RES * 60,
        .double_buffer = 1,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,     /* 回退路径：缓冲放 PSRAM，省内部 RAM */
            .sw_rotate = false,      /* 面板原生横向，无需软件旋转 */
        },
    };

    s_disp = bsp_display_start_with_config(&cfg);
    if (s_disp == NULL) {
        st->ok = false;
        snprintf(st->detail, sizeof(st->detail), "init failed");
        ESP_LOGE(TAG, "display init failed");
        return ESP_FAIL;
    }

    /* 先点亮背光，再写入目标亮度；否则 BSP 的 backlight_on 会把亮度覆盖成 100%。 */
    bsp_display_backlight_on();
    bsp_display_brightness_set(s_brightness);

    st->ok = true;
    snprintf(st->detail, sizeof(st->detail), "1024x600 JD9165");

    /* 触摸设备由 BSP 在 display 初始化时一并注册 */
    hw_status_t *t = &g_hw[HW_TOUCH];
    t->enabled = true;
    if (bsp_display_get_input_dev() != NULL) {
        t->ok = true;
        snprintf(t->detail, sizeof(t->detail), "GT911 ready");
    } else {
        t->ok = false;
        snprintf(t->detail, sizeof(t->detail), "not found");
    }

    ESP_LOGI(TAG, "display + touch ready, brightness=%d%% (dual DPI FB, direct mode)",
             s_brightness);
    return ESP_OK;
}

void hw_display_set_brightness(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    /* Repeated no-op writes reconfigure the backlight PWM and are visible as a
     * brief flicker when scripts replay the same value. */
    if (percent == s_brightness) {
        return;
    }
    s_brightness = percent;
    bsp_display_brightness_set(percent);
}

int hw_display_get_brightness(void)
{
    return s_brightness;
}

void hw_display_set_backlight(bool on)
{
    if (on) {
        bsp_display_backlight_on();
        /* BSP 的 backlight_on 会把亮度拉到 100%，这里再写回用户设置值 */
        bsp_display_brightness_set(s_brightness);
    } else {
        bsp_display_backlight_off();
    }
}
