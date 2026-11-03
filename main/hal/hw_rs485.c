#include <stdio.h>

#include "hw.h"
#include "claw_hw_registry.h"
#include "claw_port.h"
#include "esp_log.h"
#include "driver/uart.h"

static const char *TAG = "hw.rs485";

#define RS485_UART          UART_NUM_1
#define RS485_BAUD          115200

/*
 * The board's RS485 transceiver turns the UART into a half-duplex bus and
 * handles DE/RE direction automatically, so from the firmware's point of view
 * it is a plain UART.
 *
 * Official ESP-Claw hardware model: the Lua `uart` module owns the port on
 * demand (uart.new(1, 26, 27, 115200)) and releases it on close. This function
 * therefore only probes the port once so the status page can report the
 * wiring; it does not keep the driver installed.
 */
static void rs485_register_lua_resources(void)
{
    char bus_key[CLAW_HW_KEY_SIZE];
    char tx_key[CLAW_HW_KEY_SIZE];
    char rx_key[CLAW_HW_KEY_SIZE];
    const char *keys[] = {
        claw_hw_key_uart_bus(bus_key, sizeof(bus_key), RS485_UART),
        claw_hw_key_gpio(tx_key, sizeof(tx_key), PIN_RS485_TX),
        claw_hw_key_gpio(rx_key, sizeof(rx_key), PIN_RS485_RX),
    };

    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (keys[i] == NULL) {
            continue;
        }
        esp_err_t err = claw_port_add_hw_resource(keys[i], false);

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "queue hw resource '%s' failed: %s", keys[i], esp_err_to_name(err));
        }
    }
}

esp_err_t hw_rs485_init(void)
{
    hw_status_t *st = &g_hw[HW_RS485];
    const uart_config_t cfg = {
        .baud_rate = RS485_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    st->enabled = true;
    rs485_register_lua_resources();

    esp_err_t err = uart_driver_install(RS485_UART, 256, 0, 0, NULL, 0);
    if (err == ESP_OK) {
        err = uart_param_config(RS485_UART, &cfg);
    }
    if (err == ESP_OK) {
        err = uart_set_pin(RS485_UART, PIN_RS485_TX, PIN_RS485_RX,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK) {
        st->ok = true;
        snprintf(st->detail, sizeof(st->detail), "UART1 26/27 (Lua)");
        ESP_LOGI(TAG, "rs485 ready: TX=%d RX=%d @%d; open with uart.new(1, %d, %d, %d)",
                 PIN_RS485_TX, PIN_RS485_RX, RS485_BAUD,
                 PIN_RS485_TX, PIN_RS485_RX, RS485_BAUD);
    } else {
        st->ok = false;
        snprintf(st->detail, sizeof(st->detail), "probe: %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "rs485 probe failed: %s", esp_err_to_name(err));
    }

    /* Release the port for the Lua `uart` module. */
    uart_driver_delete(RS485_UART);
    return err;
}
