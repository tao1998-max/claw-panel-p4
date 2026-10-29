#include "web_ui.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "bsp/esp-bsp.h"
#include "cap_im_wechat.h"
#include "claw_storage.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_server.h"
#include "hw.h"

static const char *TAG = "web_ui";
static bool s_started;

static esp_err_t web_load_config(app_config_t *config)
{
    return app_config_load(config);
}

static esp_err_t web_save_config(const app_config_t *config)
{
    return app_config_save(config);
}

static esp_err_t web_get_wifi_status(http_server_wifi_status_t *status)
{
    if (!status) {
        return ESP_ERR_INVALID_ARG;
    }
    status->wifi_connected = hw_wifi_is_connected();
    status->ip = hw_wifi_ip();
    status->ap_active = false;
    status->ap_ssid = "";
    status->ap_ip = "";
    status->wifi_mode = "sta";
    return ESP_OK;
}

static esp_err_t web_wechat_login_start(const char *account_id, bool force)
{
    return cap_im_wechat_qr_login_start(account_id, force);
}

static esp_err_t web_wechat_login_get_status(http_server_wechat_login_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    cap_im_wechat_qr_login_status_t src = {0};
    esp_err_t err = cap_im_wechat_qr_login_get_status(&src);
    if (err != ESP_OK) {
        return err;
    }
    memset(status, 0, sizeof(*status));
    status->active = src.active;
    status->configured = src.configured;
    status->completed = src.completed;
    status->persisted = src.persisted;
    strlcpy(status->session_key, src.session_key, sizeof(status->session_key));
    strlcpy(status->status, src.status, sizeof(status->status));
    strlcpy(status->message, src.message, sizeof(status->message));
    strlcpy(status->qr_data_url, src.qr_data_url, sizeof(status->qr_data_url));
    strlcpy(status->account_id, src.account_id, sizeof(status->account_id));
    strlcpy(status->user_id, src.user_id, sizeof(status->user_id));
    strlcpy(status->token, src.token, sizeof(status->token));
    strlcpy(status->base_url, src.base_url, sizeof(status->base_url));
    return ESP_OK;
}

static esp_err_t web_wechat_login_cancel(void)
{
    return cap_im_wechat_qr_login_cancel();
}

static esp_err_t web_wechat_login_mark_persisted(void)
{
    return cap_im_wechat_qr_login_mark_persisted();
}

static void web_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t web_restart_device(void)
{
    if (xTaskCreate(web_restart_task, "web_restart", 3072, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void web_ui_start(void)
{
    if (s_started) {
        return;
    }
    const http_server_config_t config = {
        .storage_base_path = "/fatfs/claw",
        .fat_mount_point = CLAW_STORAGE_MOUNT_POINT,
        .sd_mount_point = BSP_SD_MOUNT_POINT,
        .services = {
            .load_config = web_load_config,
            .save_config = web_save_config,
            .get_wifi_status = web_get_wifi_status,
            .restart_device = web_restart_device,
            .wechat_login_start = web_wechat_login_start,
            .wechat_login_get_status = web_wechat_login_get_status,
            .wechat_login_cancel = web_wechat_login_cancel,
            .wechat_login_mark_persisted = web_wechat_login_mark_persisted,
        },
    };
    esp_err_t err = http_server_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http_server_init failed: %s", esp_err_to_name(err));
        return;
    }
    err = http_server_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http_server_start failed: %s", esp_err_to_name(err));
        return;
    }
    /* The official out_message router rule also matches Web chat replies.
     * Bind the local IM outbound callback so local_send_message returns OK
     * instead of ESP_ERR_INVALID_STATE; the Web UI itself receives the text
     * through the session/history API. */
    esp_err_t bind_err = http_server_webim_bind_im();
    if (bind_err != ESP_OK) {
        ESP_LOGW(TAG, "WebIM outbound binding failed: %s", esp_err_to_name(bind_err));
    }
    s_started = true;
    const char *ip = hw_wifi_ip();
    ESP_LOGI(TAG, "Web UI: http://%s/", (ip && ip[0]) ? ip : "device-ip");
}
