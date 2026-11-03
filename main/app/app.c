#include "app.h"

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"

#include "ai.h"
#include "ai_audio.h"
#include "app_config.h"
#include "cap_lua.h"
#include "claw_panel_hooks.h"
#include "claw_port.h"
#include "claw_tts.h"
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
    /* Network init (Wi-Fi/C6 and Ethernet PHY) is deliberately not here:
     * Ethernet takes ~4.7 s and would otherwise serialize the whole boot.
     * It runs in network_init_task() in parallel with the UI/Claw startup. */
    hw_audio_init();
    hw_storage_init();
    hw_rs485_init();
    hw_camera_init();
}

static void network_init_task(void *arg)
{
    (void)arg;
    /* Wi-Fi first: the C6 SDIO transport + association + DHCP is the long
     * pole for cloud/IM features. Ethernet follows; its PHY reset no longer
     * blocks the UI, app grid, or Claw. */
    hw_wifi_init();
    hw_eth_init();
    vTaskDeleteWithCaps(NULL);
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

/* Claw TTS shares the codec with XiaoZhi. Mute the assistant microphone
 * while Claw speaks so wake word/ASR cannot hear the reply and issue commands
 * (the previous crosstalk changed the volume mid-sentence). */
static void tts_activity(bool active)
{
    ai_set_external_audio_active(active);
}

static bool tts_play_pcm(const int16_t *pcm, size_t samples, uint32_t timeout_ms)
{
    if (pcm == NULL || samples == 0) {
        return false;
    }
    if (ai_is_busy()) {
        return false;
    }
    if (ai_audio_available()) {
        return ai_audio_write_pcm(pcm, samples, timeout_ms);
    }

    /* XiaoZhi is disabled: the assistant AudioService is not running, so use
     * the ES8311 exclusive path at the same 16 kHz rate. */
    if (ai_audio_init() != ESP_OK) {
        return false;
    }
    if (!ai_audio_exclusive_begin(16000, 2)) {
        return false;
    }
    bool ok = true;
    static int16_t stereo[2048];
    size_t offset = 0;
    while (offset < samples) {
        size_t count = samples - offset;
        if (count > 1024) {
            count = 1024;
        }
        for (size_t i = 0; i < count; i++) {
            stereo[2 * i] = pcm[offset + i];
            stereo[2 * i + 1] = pcm[offset + i];
        }
        if (!ai_audio_exclusive_write(stereo, (int)count)) {
            ok = false;
            break;
        }
        offset += count;
    }
    ai_audio_exclusive_end();
    return ok;
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

/* IM gateways need DNS and an outbound socket. Starting them in app_start()
 * happens before Wi-Fi gets an IP, so QQ/Feishu/TG log failed DNS/token
 * requests and retry for several seconds. Wait for Wi-Fi or Ethernet to get
 * an address, then apply the persisted IM credentials. */
static void im_start_task(void *arg)
{
    app_config_t *config = (app_config_t *)arg;

    /* hw_wifi_is_connected() turns true on STA link-up, before DHCP gives an
     * address. Require a non-empty Wi-Fi IP, or an Ethernet lease. */
    while (!(hw_wifi_is_connected() && hw_wifi_ip()[0]) && !hw_eth_has_ip()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    /* Let the netif/DNS state settle after the DHCP event. */
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "network ready, starting IM gateways");
    (void)app_config_apply_im(config);
    vTaskDeleteWithCaps(NULL);
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
    claw_tts_set_playback(tts_play_pcm);
    claw_tts_set_activity_callback(tts_activity);

    /* 先初始化显示与触摸并创建首屏，其余状态由 LVGL 定时器刷新。 */
    if (hw_display_init() == ESP_OK) {
        bsp_display_lock(0);
        ui_init();
        bsp_display_unlock();
    } else {
        ESP_LOGE(TAG, "Display initialization failed; running headless");
    }
    log_internal("display");

    /* The UI is already alive; bring up networking concurrently instead of
     * blocking app_start() on the C6 SDIO handshake and the Ethernet PHY. */
    if (xTaskCreateWithCaps(network_init_task, "net_init", 8192, NULL, 3,
                            NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGW(TAG, "network init task creation failed, falling back to sync");
        hw_wifi_init();
        hw_eth_init();
    }

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
    } else {
        esp_err_t audio_cleanup_err = cap_lua_register_exit_cleanup(lua_audio_exit_cleanup);
        if (audio_cleanup_err != ESP_OK) {
            ESP_LOGW(TAG, "audio Lua cleanup registration failed: %s",
                     esp_err_to_name(audio_cleanup_err));
        }
    }
    /* Apps and app fonts may live on the SD card (<mount>/apps); the scan skips
     * the root when no card is mounted, so the home page is simply empty
     * without one. */
    esp_err_t claw_err = claw_port_init(BSP_SD_MOUNT_POINT);
    if (claw_err != ESP_OK) {
        ESP_LOGW(TAG, "Claw runtime not available: %s", esp_err_to_name(claw_err));
    } else if (web_config_ok) {
        /* IM gateways need an IP for DNS/TLS; start them from a task after
         * Wi-Fi or Ethernet is actually online. */
        if (xTaskCreateWithCaps(im_start_task, "im_start", 8192, &web_config, 3,
                                NULL,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            ESP_LOGW(TAG, "IM start task creation failed");
        }
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
