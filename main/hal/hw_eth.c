#include <stdio.h>
#include <string.h>

#include "hw.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_eth_mac_esp.h"
#include "esp_eth_netif_glue.h"

static const char *TAG = "hw.eth";

static esp_eth_handle_t s_eth_handle;

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "link up");
        g_hw[HW_ETH].ok = true;
        snprintf(g_hw[HW_ETH].detail, sizeof(g_hw[HW_ETH].detail), "link up, DHCP...");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "link down");
        g_hw[HW_ETH].ok = false;
        snprintf(g_hw[HW_ETH].detail, sizeof(g_hw[HW_ETH].detail), "no cable");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "started");
        break;
    default:
        break;
    }
}

static void got_ip_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&ev->ip_info.ip));
    g_hw[HW_ETH].ok = true;
    snprintf(g_hw[HW_ETH].detail, sizeof(g_hw[HW_ETH].detail),
             "IP " IPSTR, IP2STR(&ev->ip_info.ip));
}

esp_err_t hw_eth_init(void)
{
    hw_status_t *st = &g_hw[HW_ETH];
    st->enabled = true;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        st->ok = false;
        snprintf(st->detail, sizeof(st->detail), "netif err");
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        st->ok = false;
        snprintf(st->detail, sizeof(st->detail), "event loop err");
        return err;
    }

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
    if (eth_netif == NULL) {
        st->ok = false;
        snprintf(st->detail, sizeof(st->detail), "netif new failed");
        return ESP_FAIL;
    }

    /* IP101 RMII：时钟由 PHY 输入到 GPIO50，MDC/MDIO 走 31/52，GPIO51 为 PHY 电源 */
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_gpio.mdc_num  = PIN_ETH_MDC;
    emac_cfg.smi_gpio.mdio_num = PIN_ETH_MDIO;

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    if (mac == NULL) {
        st->ok = false;
        snprintf(st->detail, sizeof(st->detail), "mac new failed");
        return ESP_FAIL;
    }

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = ETH_PHY_ADDR;
    phy_cfg.reset_gpio_num = PIN_ETH_PHY_RST;
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_cfg);
    if (phy == NULL) {
        st->ok = false;
        snprintf(st->detail, sizeof(st->detail), "phy new failed");
        return ESP_FAIL;
    }

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    err = esp_eth_driver_install(&eth_cfg, &s_eth_handle);
    if (err != ESP_OK) {
        st->ok = false;
        snprintf(st->detail, sizeof(st->detail), "driver err %d", err);
        return err;
    }

    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(s_eth_handle);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, glue));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip_handler, NULL));

    err = esp_eth_start(s_eth_handle);
    if (err != ESP_OK) {
        st->ok = false;
        snprintf(st->detail, sizeof(st->detail), "start err %d", err);
        return err;
    }

    st->ok = true;
    snprintf(st->detail, sizeof(st->detail), "IP101 waiting link");
    ESP_LOGI(TAG, "ip101 started (mdc=%d mdio=%d rst=%d)", PIN_ETH_MDC, PIN_ETH_MDIO, PIN_ETH_PHY_RST);
    return ESP_OK;
}
