#include <stdio.h>
#include <string.h>

#include "hw.h"
#include "esp_log.h"

#if CONFIG_TEMPLATE_ENABLE_WIFI

#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"

static const char *TAG = "hw.wifi";

static bool s_has_credentials;
static bool s_connected;
static char s_ssid[33];
static char s_ip[16];

static bool s_scan_running;
static int s_scan_count;
static hw_wifi_ap_t s_aps[HW_WIFI_AP_MAX];

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_has_credentials) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_ip[0] = '\0';
        g_hw[HW_WIFI].ok = false;
        snprintf(g_hw[HW_WIFI].detail, sizeof(g_hw[HW_WIFI].detail), "reconnecting");
        if (s_has_credentials) {
            ESP_LOGW(TAG, "disconnected, retrying...");
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        s_connected = true;
        g_hw[HW_WIFI].ok = true;
        snprintf(g_hw[HW_WIFI].detail, sizeof(g_hw[HW_WIFI].detail), "connected");
        ESP_LOGI(TAG, "link up");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        /* 事件回调跑在 sys_evt 任务里，栈很小（默认 2~3 KB），
         * 16 条 wifi_ap_record_t 放栈上会直接栈溢出，必须用 static。 */
        static wifi_ap_record_t records[HW_WIFI_AP_MAX];
        uint16_t num = HW_WIFI_AP_MAX;

        memset(records, 0, sizeof(records));

        if (esp_wifi_scan_get_ap_records(&num, records) != ESP_OK) {
            ESP_LOGW(TAG, "scan_get_ap_records failed");
            num = 0;
        }
        /* 去重：同名 SSID 只留信号最强的一条 */
        s_scan_count = 0;
        for (int i = 0; i < num && s_scan_count < HW_WIFI_AP_MAX; i++) {
            if (records[i].ssid[0] == '\0') {
                continue;
            }
            bool dup = false;
            for (int j = 0; j < s_scan_count; j++) {
                if (strcmp(s_aps[j].ssid, (const char *)records[i].ssid) == 0) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            snprintf(s_aps[s_scan_count].ssid, sizeof(s_aps[s_scan_count].ssid), "%s",
                     (const char *)records[i].ssid);
            s_aps[s_scan_count].rssi = records[i].rssi;
            s_aps[s_scan_count].secure = (records[i].authmode != WIFI_AUTH_OPEN);
            s_scan_count++;
        }
        s_scan_running = false;
        ESP_LOGI(TAG, "scan done: %d AP(s)", s_scan_count);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        s_connected = true;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&ev->ip_info.ip));
        g_hw[HW_WIFI].ok = true;
        snprintf(g_hw[HW_WIFI].detail, sizeof(g_hw[HW_WIFI].detail),
                 "IP " IPSTR, IP2STR(&ev->ip_info.ip));

        /* 历史曲线横轴要显示日期时间；板子没有 RTC，联网后校时一次。 */
        hw_sntp_start_once();
    }
}

static esp_err_t wifi_fail(hw_status_t *st, esp_err_t err, const char *step)
{
    st->ok = false;
    snprintf(st->detail, sizeof(st->detail), "%s err %d", step, err);
    ESP_LOGE(TAG, "%s failed: %s", step, esp_err_to_name(err));
    return err;
}

esp_err_t hw_wifi_init(void)
{
    hw_status_t *st = &g_hw[HW_WIFI];
    st->enabled = true;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return wifi_fail(st, err, "netif");
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return wifi_fail(st, err, "event");
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        return wifi_fail(st, err, "wifi init");
    }

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                wifi_event_handler, NULL));

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return wifi_fail(st, err, "set mode");
    }

    wifi_config_t wc = { 0 };
    if (strlen(CONFIG_TEMPLATE_WIFI_SSID) > 0) {
        strncpy((char *)wc.sta.ssid, CONFIG_TEMPLATE_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
        strncpy((char *)wc.sta.password, CONFIG_TEMPLATE_WIFI_PASSWORD,
                sizeof(wc.sta.password) - 1);
        s_has_credentials = true;
        ESP_LOGI(TAG, "using SSID from menuconfig: %s", CONFIG_TEMPLATE_WIFI_SSID);
    } else if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK && wc.sta.ssid[0] != '\0') {
        /* 没在 menuconfig 填账号时，复用 NVS 里之前保存的配网信息。 */
        s_has_credentials = true;
        ESP_LOGI(TAG, "using stored SSID from NVS: %s", (char *)wc.sta.ssid);
    }

    if (s_has_credentials) {
        wc.sta.threshold.authmode = (strlen((char *)wc.sta.password) == 0)
                                    ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
        err = esp_wifi_set_config(WIFI_IF_STA, &wc);
        if (err != ESP_OK) {
            return wifi_fail(st, err, "set config");
        }
        snprintf(s_ssid, sizeof(s_ssid), "%s", (const char *)wc.sta.ssid);
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        return wifi_fail(st, err, "start");
    }

    if (s_has_credentials) {
        st->ok = true;
        snprintf(st->detail, sizeof(st->detail), "connecting...");
        ESP_LOGI(TAG, "wifi test started, connecting via ESP32-C6");
    } else {
        st->ok = false;
        snprintf(st->detail, sizeof(st->detail), "no SSID configured");
        ESP_LOGW(TAG, "no SSID in menuconfig or NVS; configure Wi-Fi and reboot");
    }
    return ESP_OK;
}

/* ---------------- 运行期配置（设置页用） ---------------- */

void hw_wifi_scan_start(void)
{
    if (s_scan_running) {
        return;
    }
    wifi_scan_config_t scan_cfg = { 0 };
    s_scan_count = 0;
    s_scan_running = true;
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        s_scan_running = false;
        ESP_LOGW(TAG, "scan start failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "scan started");
    }
}

bool hw_wifi_scan_running(void)
{
    return s_scan_running;
}

int hw_wifi_scan_count(void)
{
    return s_scan_count;
}

const hw_wifi_ap_t *hw_wifi_scan_ap(int index)
{
    if (index < 0 || index >= s_scan_count) {
        return NULL;
    }
    return &s_aps[index];
}

void hw_wifi_connect(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return;
    }

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    if (password != NULL) {
        strncpy((char *)wc.sta.password, password, sizeof(wc.sta.password) - 1);
    }
    wc.sta.threshold.authmode = (password != NULL && password[0] != '\0')
                                ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set config failed: %s", esp_err_to_name(err));
        return;
    }

    s_has_credentials = true;
    s_connected = false;
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    g_hw[HW_WIFI].ok = false;
    snprintf(g_hw[HW_WIFI].detail, sizeof(g_hw[HW_WIFI].detail), "connecting...");

    esp_wifi_disconnect();
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "connecting to %s", ssid);
}

void hw_wifi_disconnect(void)
{
    s_has_credentials = false;
    s_connected = false;
    s_ip[0] = '\0';
    esp_wifi_disconnect();
    g_hw[HW_WIFI].ok = false;
    snprintf(g_hw[HW_WIFI].detail, sizeof(g_hw[HW_WIFI].detail), "disconnected");
    ESP_LOGI(TAG, "disconnected by user");
}

bool hw_wifi_is_connected(void)
{
    return s_connected;
}

const char *hw_wifi_sta_ssid(void)
{
    return s_ssid;
}

const char *hw_wifi_ip(void)
{
    return s_ip;
}

int hw_wifi_get_rssi(void)
{
    if (!s_connected) {
        return -127;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return -127;
}

#else  /* !CONFIG_TEMPLATE_ENABLE_WIFI */

static const char *TAG = "hw.wifi";

esp_err_t hw_wifi_init(void)
{
    hw_status_t *st = &g_hw[HW_WIFI];
    st->enabled = false;
    st->ok = false;
    snprintf(st->detail, sizeof(st->detail), "disabled");
    ESP_LOGI(TAG, "wifi disabled (CONFIG_TEMPLATE_ENABLE_WIFI=n)");
    return ESP_ERR_NOT_SUPPORTED;
}

void hw_wifi_scan_start(void) {}
bool hw_wifi_scan_running(void) { return false; }
int hw_wifi_scan_count(void) { return 0; }
const hw_wifi_ap_t *hw_wifi_scan_ap(int index) { (void)index; return NULL; }
void hw_wifi_connect(const char *ssid, const char *password) { (void)ssid; (void)password; }
void hw_wifi_disconnect(void) {}
bool hw_wifi_is_connected(void) { return false; }
const char *hw_wifi_sta_ssid(void) { return ""; }
const char *hw_wifi_ip(void) { return ""; }
int hw_wifi_get_rssi(void) { return -127; }

#endif
