#include <stdio.h>
#include <string.h>

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
            .task_stack = 16384,   /* 全屏 Aurora 背景图绘制比纯渐变占栈 */
            .task_affinity = -1,
            .task_max_sleep_ms = 500,
            .timer_period_ms = 5,
        },
        .buffer_size = BSP_LCD_H_RES * 60,
        .double_buffer = 1,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,     /* 缓冲放 PSRAM，省内部 RAM */
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

    ESP_LOGI(TAG, "display + touch ready, brightness=%d%%", s_brightness);
    return ESP_OK;
}

void hw_display_set_brightness(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
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
