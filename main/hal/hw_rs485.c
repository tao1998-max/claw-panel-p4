#include <stdio.h>
#include <string.h>

#include "hw.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hw.rs485";

#define RS485_UART          UART_NUM_1
#define RS485_BAUD          115200
#define RS485_BUF_SIZE      1024

static bool s_inited;
static uint32_t s_rx_count;
static uint32_t s_tx_count;

static void rs485_task(void *arg)
{
    uint8_t buf[256];

    while (1) {
        int n = uart_read_bytes(RS485_UART, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (n <= 0) {
            continue;
        }
        s_rx_count += n;

        if (CONFIG_TEMPLATE_RS485_ECHO) {
            uart_write_bytes(RS485_UART, buf, n);
            s_tx_count += n;
        }

        ESP_LOGI(TAG, "rx %d bytes: %.*s", n, n, (char *)buf);
    }
}

esp_err_t hw_rs485_init(void)
{
    hw_status_t *st = &g_hw[HW_RS485];
    st->enabled = true;

    if (s_inited) {
        st->ok = true;
        return ESP_OK;
    }

    const uart_config_t cfg = {
        .baud_rate = RS485_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(RS485_UART, RS485_BUF_SIZE, RS485_BUF_SIZE, 0, NULL, 0);
    if (err != ESP_OK) {
        st->ok = false;
        snprintf(st->detail, sizeof(st->detail), "install err %d", err);
        return err;
    }

    ESP_ERROR_CHECK(uart_param_config(RS485_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(RS485_UART, PIN_RS485_TX, PIN_RS485_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    s_inited = true;
    st->ok = true;
    snprintf(st->detail, sizeof(st->detail), "UART1 %d baud", RS485_BAUD);

    xTaskCreate(rs485_task, "rs485", 4096, NULL, 6, NULL);

    ESP_LOGI(TAG, "rs485 ready: TX=%d RX=%d @%d", PIN_RS485_TX, PIN_RS485_RX, RS485_BAUD);
    return ESP_OK;
}
