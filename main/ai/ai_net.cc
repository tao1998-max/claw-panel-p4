#include "ai_net.h"

#include <atomic>

#include "esp_event.h"
#include "esp_eth.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
#include "hw.h"
}

#include "esp_network.h"

static const char *TAG = "ai.net";

static EspNetwork s_network;
static std::atomic<bool> s_started{false};
static std::atomic<bool> s_online{false};
static std::function<void(bool)> s_callback;

static bool netif_has_ip(const char *key)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(key);
    if (netif == NULL) {
        return false;
    }
    esp_netif_ip_info_t ip;
    return esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0;
}

static bool refresh_online(void)
{
    bool online = netif_has_ip("WIFI_STA_DEF") || netif_has_ip("ETH_DEF");
    bool was_online = s_online.exchange(online);
    if (online) {
        hw_sntp_start_once();
    }
    if (online != was_online && s_callback) {
        s_callback(online);
    }
    return online;
}

static void net_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;

    if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP || id == IP_EVENT_ETH_GOT_IP) {
            refresh_online();
        } else if (id == IP_EVENT_STA_LOST_IP || id == IP_EVENT_ETH_LOST_IP) {
            refresh_online();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        refresh_online();
    } else if (base == ETH_EVENT && id == ETHERNET_EVENT_DISCONNECTED) {
        refresh_online();
    }
}

NetworkInterface *ai_net_get(void)
{
    return &s_network;
}

void ai_net_start(void)
{
    if (s_started.exchange(true)) {
        return;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "esp_netif_init: %s", esp_err_to_name(err));
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "event loop: %s", esp_err_to_name(err));
    }

    esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, net_event_handler, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, net_event_handler, NULL);
    esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, net_event_handler, NULL);

    refresh_online();
    ESP_LOGI(TAG, "network adapter ready, online=%d", s_online.load());
}

bool ai_net_is_online(void)
{
    return refresh_online();
}

bool ai_net_wait_online(int timeout_ms)
{
    if (ai_net_is_online()) {
        return true;
    }
    int waited = 0;
    while (waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
        if (ai_net_is_online()) {
            return true;
        }
    }
    return false;
}

void ai_net_set_callback(std::function<void(bool online)> callback)
{
    s_callback = std::move(callback);
}
