#include "app.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"

#include "hw.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "app";

static void nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

static void peripherals_init(void)
{
    /* 显示已在首屏创建前初始化；其余模块失败后继续启动其它外设。 */
    hw_audio_init();
    hw_storage_init();
    hw_rs485_init();
    hw_eth_init();
    hw_camera_init();
    hw_wifi_init();
}

static void log_hardware_summary(void)
{
    ESP_LOGI(TAG, "================ HARDWARE SUMMARY ================");
    for (int i = 0; i < HW_MODULE_COUNT; i++) {
        ESP_LOGI(TAG, "%-8s : %-4s %s", hw_name((hw_module_t)i),
                 g_hw[i].enabled ? (g_hw[i].ok ? "OK" : "FAIL") : "OFF",
                 g_hw[i].detail);
    }
}

void app_start(void)
{
    ESP_LOGI(TAG, "P4 HMI template v%s", esp_app_get_description()->version);
    ESP_LOGI(TAG, "Board: JC1060P470C_I_W_Y / ESP32-P4 + ESP32-C6");

    nvs_init();
    /* 只读参数，不碰硬件：此时显示和音频都还没初始化 */
    settings_init();

    /* 先初始化显示与触摸并创建首屏，其余状态由 LVGL 定时器刷新。 */
    if (hw_display_init() == ESP_OK) {
        bsp_display_lock(0);
        ui_init();
        bsp_display_unlock();
    } else {
        ESP_LOGE(TAG, "Display initialization failed; running headless");
    }

    peripherals_init();

    /* 外设都起来了，把亮度/音量/语言推到硬件 */
    settings_apply();

    log_hardware_summary();
}
