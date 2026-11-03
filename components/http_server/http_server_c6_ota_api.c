/*
 * ESP-Hosted C6 slave firmware OTA.
 *
 * The host (ESP32-P4) transfers a network_adapter.bin image to the C6 over
 * the existing SDIO link, so no C6 UART/ESP-Prog is needed. The image is
 * read from the SD card and must be built from the esp_hosted slave project
 * with the same version as the host component.
 */
#include "http_server_priv.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_hosted.h"
#include "esp_hosted_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#define TAG "http_c6_ota"
#define C6_OTA_PATH "/sdcard/c6/network_adapter.bin"
#define C6_OTA_CHUNK 1500
#define C6_OTA_TASK_STACK 8192

static bool s_c6_ota_running;

static void c6_ota_task(void *arg)
{
    (void)arg;
    uint8_t *chunk = NULL;
    FILE *firmware = NULL;
    esp_err_t err = ESP_FAIL;
    size_t total = 0;

    /* Give the HTTP handler time to flush its 202 response. */
    vTaskDelay(pdMS_TO_TICKS(500));

    firmware = fopen(C6_OTA_PATH, "rb");
    if (firmware == NULL) {
        ESP_LOGE(TAG, "cannot open %s", C6_OTA_PATH);
        goto done;
    }
    chunk = heap_caps_malloc(C6_OTA_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (chunk == NULL) {
        ESP_LOGE(TAG, "no memory for OTA chunk");
        goto done;
    }

    esp_hosted_coprocessor_fwver_t current = {0};
    if (esp_hosted_get_coprocessor_fwversion(&current) == ESP_OK) {
        ESP_LOGI(TAG, "current C6 firmware %u.%u.%u",
                 (unsigned)current.major1, (unsigned)current.minor1,
                 (unsigned)current.patch1);
    }

    err = esp_hosted_slave_ota_begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin failed: %s", esp_err_to_name(err));
        goto done;
    }

    for (;;) {
        size_t n = fread(chunk, 1, C6_OTA_CHUNK, firmware);
        if (n == 0) {
            break;
        }
        err = esp_hosted_slave_ota_write(chunk, (uint32_t)n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota_write failed at %u: %s", (unsigned)total,
                     esp_err_to_name(err));
            goto done;
        }
        total += n;
        if ((total % (256 * 1024)) < n) {
            ESP_LOGI(TAG, "sent %u KB", (unsigned)(total / 1024));
        }
    }

    err = esp_hosted_slave_ota_end();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_end failed: %s", esp_err_to_name(err));
        goto done;
    }
    ESP_LOGI(TAG, "C6 image transferred (%u bytes)", (unsigned)total);

    /* The activate RPC is only available on slave FW >= 2.6.0. The current
     * 2.3.x slave marks the new OTA image through ota_end; a host restart
     * lets it resync. */
    if ((current.major1 > 2) ||
        (current.major1 == 2 && current.minor1 > 5)) {
        err = esp_hosted_slave_ota_activate();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ota_activate failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TAG, "activate API not supported by current C6 firmware");
    }

    ESP_LOGW(TAG, "restarting P4 to resync with C6");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

done:
    if (firmware != NULL) {
        fclose(firmware);
    }
    heap_caps_free(chunk);
    s_c6_ota_running = false;
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t c6_ota_handler(httpd_req_t *req)
{
    if (s_c6_ota_running) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"c6_ota_busy\"}");
    }

    FILE *firmware = fopen(C6_OTA_PATH, "rb");
    if (firmware == NULL) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req,
                                  "{\"ok\":false,\"error\":\"c6_firmware_missing\"}");
    }
    if (fseek(firmware, 0, SEEK_END) != 0) {
        fclose(firmware);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"c6_file_error\"}");
    }
    long size = ftell(firmware);
    fclose(firmware);
    if (size <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"c6_file_empty\"}");
    }

    s_c6_ota_running = true;
    if (xTaskCreateWithCaps(c6_ota_task, "c6_ota", C6_OTA_TASK_STACK, NULL, 5,
                            NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_c6_ota_running = false;
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"c6_task_failed\"}");
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return httpd_resp_send_500(req);
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "message", "C6 OTA started");
    cJSON_AddNumberToObject(root, "bytes", (double)size);
    httpd_resp_set_status(req, "202 Accepted");
    return http_server_send_json_response(req, root);
}

esp_err_t http_server_register_c6_ota_routes(httpd_handle_t server)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/api/c6-ota", .method = HTTP_POST, .handler = c6_ota_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}
