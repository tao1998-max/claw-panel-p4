#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* ==========================================================================
 *  JC1060P470C_I_W_Y  (ESP32-P4 + ESP32-C6, 7" 1024x600 IPS 电容触摸)
 *  统一硬件抽象层
 *
 *  显示/触摸/音频/SD/摄像头 走厂商 BSP（esp32_p4_function_ev_board）
 *  RS485 / 以太网 由本工程直接驱动（BSP 未覆盖）
 * ========================================================================== */

/* ---------- 本工程直接使用的引脚 ---------- */
#define PIN_RS485_TX        26
#define PIN_RS485_RX        27

#define PIN_ETH_MDC         31
#define PIN_ETH_MDIO        52
#define PIN_ETH_PHY_RST     51
#define PIN_ETH_RMII_CLK    50      /* PHY 提供 50MHz RMII 时钟输入 */
#define ETH_PHY_ADDR        1

/* ---------- BSP 托管的引脚（备查，不要重复初始化）---------- */
#define PIN_LCD_BACKLIGHT   23
#define PIN_I2C_SDA         7
#define PIN_I2C_SCL         8
#define PIN_I2S_MCLK        13
#define PIN_I2S_BCLK        12
#define PIN_I2S_WS          10
#define PIN_I2S_DOUT        9
#define PIN_I2S_DIN         48
#define PIN_PA_CTRL         11
#define PIN_SD_CLK          43
#define PIN_SD_CMD          44
#define PIN_SD_D0           39
#define PIN_SD_D1           40
#define PIN_SD_D2           41
#define PIN_SD_D3           42
#define PIN_C6_SDIO_CLK     18
#define PIN_C6_SDIO_CMD     19
#define PIN_C6_SDIO_D0      14
#define PIN_C6_SDIO_D1      15
#define PIN_C6_SDIO_D2      16
#define PIN_C6_SDIO_D3      17
#define PIN_C6_RESET        54

/* ---------- 模块状态 ---------- */
typedef enum {
    HW_DISPLAY = 0,
    HW_TOUCH,
    HW_AUDIO,
    HW_SDCARD,
    HW_RS485,
    HW_ETH,
    HW_CAMERA,
    HW_WIFI,
    HW_MODULE_COUNT
} hw_module_t;

typedef struct {
    bool ok;            /* 初始化是否成功 */
    bool enabled;       /* 本固件是否编译/启用该模块 */
    char detail[40];    /* 给 UI 显示的简短描述 */
} hw_status_t;

extern hw_status_t g_hw[HW_MODULE_COUNT];

const char *hw_name(hw_module_t m);

/* ---------- 初始化（返回 ESP_OK 表示硬件就绪）---------- */
esp_err_t hw_display_init(void);   /* 同时初始化触摸 */
esp_err_t hw_audio_init(void);
esp_err_t hw_storage_init(void);
esp_err_t hw_rs485_init(void);
esp_err_t hw_eth_init(void);
esp_err_t hw_camera_init(void);
esp_err_t hw_wifi_init(void);

/* ---------- 运行期控制 ---------- */
void hw_audio_set_volume(int percent);
int  hw_audio_get_volume(void);
void hw_audio_beep(void);

/* 任一网口拿到 IP 后只启动一次 SNTP（Wi-Fi / 以太网 / AI 共用）。 */
void hw_sntp_start_once(void);

void hw_display_set_brightness(int percent);
int  hw_display_get_brightness(void);

/* 背光开关（息屏用）。on 时会恢复当前亮度，BSP 的 backlight_on 会强制 100%。 */
void hw_display_set_backlight(bool on);

/* ---------- Wi-Fi 运行期配置（设置页用） ---------- */
#define HW_WIFI_AP_MAX  16

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} hw_wifi_ap_t;

void hw_wifi_scan_start(void);
bool hw_wifi_scan_running(void);
int  hw_wifi_scan_count(void);
const hw_wifi_ap_t *hw_wifi_scan_ap(int index);
void hw_wifi_connect(const char *ssid, const char *password);
void hw_wifi_disconnect(void);
bool hw_wifi_is_connected(void);
const char *hw_wifi_sta_ssid(void);
const char *hw_wifi_ip(void);
/* 当前 AP 的 RSSI（dBm）；未连接返回 -127 */
int hw_wifi_get_rssi(void);
