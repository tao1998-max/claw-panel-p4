#include "app.h"

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"

#include "ai.h"
#include "app_config.h"
#include "claw_panel_hooks.h"
#include "claw_port.h"
#include "hw.h"
#include "lua_module_audio.h"
#include "settings.h"
#include "ui.h"
#include "web_ui.h"

static const char *TAG = "app";

extern void lvgl_mem_psram_ensure_linked(void);

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

static void log_internal(const char *stage)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "internal@%s free=%uKB largest=%uKB min=%uKB", stage,
             (unsigned)(info.total_free_bytes / 1024),
             (unsigned)(info.largest_free_block / 1024),
             (unsigned)(info.minimum_free_bytes / 1024));
}

void app_start(void)
{
    lvgl_mem_psram_ensure_linked();
    ESP_LOGI(TAG, "P4 HMI template v%s", esp_app_get_description()->version);
    ESP_LOGI(TAG, "Board: JC1060P470C_I_W_Y / ESP32-P4 + ESP32-C6");

    nvs_init();
    /* 只读参数，不碰硬件：此时显示和音频都还没初始化 */
    settings_init();

    /* 小智：挂载共享 FAT、把聊天字体和唤醒词模型读进 PSRAM。
     * 必须在 ui_init() 之前，ai_ui_init() 要用字体 buffer。 */
    ai_early_init();

    /* 先初始化显示与触摸并创建首屏，其余状态由 LVGL 定时器刷新。 */
    if (hw_display_init() == ESP_OK) {
        bsp_display_lock(0);
        ui_init();
        bsp_display_unlock();
    } else {
        ESP_LOGE(TAG, "Display initialization failed; running headless");
    }
    log_internal("display");

    peripherals_init();
    log_internal("peripherals");

    /* 外设都起来了，把亮度/音量/语言推到硬件 */
    settings_apply();

    /* 时区要在 Claw/Agent 启动前生效，cron 和 Agent 的时间才正确。
     * app_config_t is large (~5 KB); keep it out of the 8 KB main-task stack. */
    static app_config_t web_config;
    bool web_config_ok = app_config_load(&web_config) == ESP_OK;
    if (web_config_ok) {
        (void)app_config_apply_timezone(&web_config);
    }
    /* Seed the system clock from the on-board RX8025T before Claw and the
     * scheduler start, so cron entries can be armed even before SNTP. */
    (void)hw_rtc_init();

    /* claw.panel.* -> 现有设置/UI（LVGL 事件队列，跨任务安全） */
    claw_panel_hooks_init();

    /* ESP-Claw 本地执行层：能力注册要在 MCP 工具注册前完成。
     * 失败只记日志，不影响小智语音主链路。 */
    /* Product extension: audio.play("<path>.ogg") for Lua apps. Queue it
     * before claw_port_init() because the Lua module registry is sealed when
     * the Claw capability groups start. */
    esp_err_t audio_mod_err = claw_port_add_lua_module("audio", luaopen_audio);
    if (audio_mod_err != ESP_OK) {
        ESP_LOGW(TAG, "audio Lua module queue failed: %s",
                 esp_err_to_name(audio_mod_err));
    }
    /* Apps and app fonts may live on the SD card (<mount>/apps); the scan skips
     * the root when no card is mounted, so the home page is simply empty
     * without one. */
    esp_err_t claw_err = claw_port_init(BSP_SD_MOUNT_POINT);
    if (claw_err != ESP_OK) {
        ESP_LOGW(TAG, "Claw runtime not available: %s", esp_err_to_name(claw_err));
    } else if (web_config_ok) {
        /* IM groups exist now; apply persisted credentials and start gateways. */
        (void)app_config_apply_im(&web_config);
    }
    log_internal("claw");

    /* AI 任务非阻塞启动：等联网后才激活/连接，不拖慢开机。 */
    ai_start();
    log_internal("ai");

    /* ESP-Claw Web 管理端：HTTP 服务绑定 0.0.0.0:80，联网后即可访问。 */
    web_ui_start();
    log_internal("web");

    log_hardware_summary();
}
