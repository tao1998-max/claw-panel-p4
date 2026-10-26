#include "ai_system_info.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "esp_random.h"
#include "esp_system.h"
#include "nvs.h"

static const char *TAG = "ai.sysinfo";

static char s_mac[18];
static char s_uuid[37];
static char s_ua[80];
static bool s_inited;

static void format_mac(const uint8_t *mac, char *out, size_t out_size)
{
    snprintf(out, out_size, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool mac_is_zero(const uint8_t *mac)
{
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0) return false;
    }
    return true;
}

static void get_stable_mac(uint8_t *mac)
{
    if (esp_read_mac(mac, ESP_MAC_BASE) != ESP_OK || mac_is_zero(mac)) {
        esp_netif_t *eth = esp_netif_get_handle_from_ifkey("ETH_DEF");
        if (eth == NULL ||
            esp_netif_get_mac(eth, mac) != ESP_OK || mac_is_zero(mac)) {
            esp_efuse_mac_get_default(mac);
        }
    }
}

static void generate_uuid(char *out, size_t out_size)
{
    uint8_t uuid[16];
    esp_fill_random(uuid, sizeof(uuid));
    uuid[6] = (uuid[6] & 0x0F) | 0x40;
    uuid[8] = (uuid[8] & 0x3F) | 0x80;
    snprintf(out, out_size,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
             uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
}

static void load_identity(void)
{
    if (s_inited) return;
    s_inited = true;

    uint8_t mac[6] = {0};
    get_stable_mac(mac);
    format_mac(mac, s_mac, sizeof(s_mac));

    nvs_handle_t h;
    esp_err_t err = nvs_open("board", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        size_t len = sizeof(s_uuid);
        err = nvs_get_str(h, "uuid", s_uuid, &len);
        if (err != ESP_OK || s_uuid[0] == '\0') {
            generate_uuid(s_uuid, sizeof(s_uuid));
            nvs_set_str(h, "uuid", s_uuid);
            nvs_commit(h);
        }
        nvs_close(h);
    } else {
        generate_uuid(s_uuid, sizeof(s_uuid));
    }

    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(s_ua, sizeof(s_ua), "JC1060P470C_I_W_Y/%s", app != NULL ? app->version : "unknown");

    ESP_LOGI(TAG, "Device-Id=%s Client-Id=%s", s_mac, s_uuid);
}

const char *ai_device_id(void)
{
    load_identity();
    return s_mac;
}

const char *ai_device_uuid(void)
{
    load_identity();
    return s_uuid;
}

const char *ai_user_agent(void)
{
    load_identity();
    return s_ua;
}

static void add_ip(cJSON *board)
{
    const char *keys[] = {"WIFI_STA_DEF", "ETH_DEF"};
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey(keys[i]);
        if (netif == NULL) continue;
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
            cJSON_AddStringToObject(board, "ip", buf);
            return;
        }
    }
}

char *ai_system_info_json(void)
{
    load_identity();

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return NULL;

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    cJSON_AddNumberToObject(root, "version", 2);
    cJSON_AddStringToObject(root, "language", "zh-CN");
    cJSON_AddNumberToObject(root, "flash_size", (double)flash_size);
    cJSON_AddNumberToObject(root, "psram_size", (double)esp_psram_get_size());
    cJSON_AddNumberToObject(root, "minimum_free_heap_size", (double)esp_get_minimum_free_heap_size());
    cJSON_AddStringToObject(root, "mac_address", s_mac);
    cJSON_AddStringToObject(root, "uuid", s_uuid);
    cJSON_AddStringToObject(root, "chip_model_name", CONFIG_IDF_TARGET);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    cJSON *chip_info = cJSON_AddObjectToObject(root, "chip_info");
    cJSON_AddNumberToObject(chip_info, "model", chip.model);
    cJSON_AddNumberToObject(chip_info, "cores", chip.cores);
    cJSON_AddNumberToObject(chip_info, "revision", chip.revision);
    cJSON_AddNumberToObject(chip_info, "features", (double)chip.features);

    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *application = cJSON_AddObjectToObject(root, "application");
    cJSON_AddStringToObject(application, "name", app->project_name);
    cJSON_AddStringToObject(application, "version", app->version);
    char compile_time[40];
    snprintf(compile_time, sizeof(compile_time), "%sT%sZ", app->date, app->time);
    cJSON_AddStringToObject(application, "compile_time", compile_time);
    cJSON_AddStringToObject(application, "idf_version", app->idf_ver);
    char sha[65] = {0};
    for (int i = 0; i < 32; i++) {
        snprintf(sha + i * 2, sizeof(sha) - i * 2, "%02x", app->app_elf_sha256[i]);
    }
    cJSON_AddStringToObject(application, "elf_sha256", sha);

    cJSON *parts = cJSON_AddArrayToObject(root, "partition_table");
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it != NULL) {
        const esp_partition_t *p = esp_partition_get(it);
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "label", p->label);
        cJSON_AddNumberToObject(item, "type", p->type);
        cJSON_AddNumberToObject(item, "subtype", p->subtype);
        cJSON_AddNumberToObject(item, "address", p->address);
        cJSON_AddNumberToObject(item, "size", p->size);
        cJSON_AddItemToArray(parts, item);
        it = esp_partition_next(it);
    }

    const esp_partition_t *app_slot = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    cJSON *ota = cJSON_AddObjectToObject(root, "ota");
    cJSON_AddStringToObject(ota, "label", app_slot != NULL ? app_slot->label : "ota_0");

    cJSON *display = cJSON_AddObjectToObject(root, "display");
    cJSON_AddBoolToObject(display, "monochrome", false);
    cJSON_AddNumberToObject(display, "width", 1024);
    cJSON_AddNumberToObject(display, "height", 600);

    cJSON *board = cJSON_AddObjectToObject(root, "board");
    cJSON_AddStringToObject(board, "type", "jc1060p470c");
    cJSON_AddStringToObject(board, "name", "p4_template");
    cJSON_AddStringToObject(board, "mac", s_mac);
    add_ip(board);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

void ai_system_info_free(char *json)
{
    cJSON_free(json);
}
