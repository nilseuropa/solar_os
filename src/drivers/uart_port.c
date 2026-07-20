#include "uart_port.h"

#include <inttypes.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "uart_port";

static uart_port_config_t active_configs[UART_NUM_MAX];
static bool ready[UART_NUM_MAX];

static bool valid_port(uart_port_t port_num)
{
    return port_num >= UART_NUM_0 && port_num < UART_NUM_MAX;
}

esp_err_t uart_port_init(const uart_port_config_t *config)
{
    if (config == NULL || !valid_port(config->port_num) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(config->tx_pin) ||
        !GPIO_IS_VALID_GPIO(config->rx_pin) ||
        config->tx_pin == config->rx_pin || config->baud_rate == 0 ||
        config->rx_buffer_size == 0 || config->tx_buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uart_config_t uart_config = {
        .baud_rate = (int)config->baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_param_config(config->port_num, &uart_config),
                        TAG,
                        "UART parameter config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(config->port_num,
                                     config->tx_pin,
                                     config->rx_pin,
                                     UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG,
                        "UART pin config failed");

    if (!ready[config->port_num]) {
        ESP_RETURN_ON_ERROR(uart_driver_install(config->port_num,
                                                (int)config->rx_buffer_size,
                                                (int)config->tx_buffer_size,
                                                0,
                                                NULL,
                                                0),
                            TAG,
                            "UART driver install failed");
        ready[config->port_num] = true;
    }

    active_configs[config->port_num] = *config;
    ESP_LOGI(TAG,
             "UART%d ready: TX=%d RX=%d baud=%" PRIu32,
             (int)config->port_num,
             (int)config->tx_pin,
             (int)config->rx_pin,
             config->baud_rate);
    return ESP_OK;
}

esp_err_t uart_port_deinit(uart_port_t port_num)
{
    if (!valid_port(port_num)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ready[port_num]) {
        return ESP_OK;
    }

    const uart_port_config_t config = active_configs[port_num];
    ESP_RETURN_ON_ERROR(uart_wait_tx_done(port_num, pdMS_TO_TICKS(100)),
                        TAG,
                        "wait for UART TX failed");
    ESP_RETURN_ON_ERROR(uart_driver_delete(port_num), TAG, "delete UART driver failed");
    ready[port_num] = false;
    active_configs[port_num] = (uart_port_config_t) {0};
    (void)gpio_reset_pin(config.tx_pin);
    (void)gpio_reset_pin(config.rx_pin);
    return ESP_OK;
}

bool uart_port_is_ready(uart_port_t port_num)
{
    return valid_port(port_num) && ready[port_num];
}

esp_err_t uart_port_set_baud_rate(uart_port_t port_num, uint32_t baud_rate)
{
    if (!uart_port_is_ready(port_num)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (baud_rate == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(uart_set_baudrate(port_num, baud_rate),
                        TAG,
                        "UART baud config failed");
    active_configs[port_num].baud_rate = baud_rate;
    return ESP_OK;
}

esp_err_t uart_port_write(uart_port_t port_num,
                          const uint8_t *data,
                          size_t len,
                          size_t *written)
{
    if (written != NULL) {
        *written = 0;
    }
    if (!uart_port_is_ready(port_num)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }

    const int ret = uart_write_bytes(port_num, data, len);
    if (ret < 0) {
        return ESP_FAIL;
    }
    if (written != NULL) {
        *written = (size_t)ret;
    }
    return (size_t)ret == len ? ESP_OK : ESP_FAIL;
}

esp_err_t uart_port_read(uart_port_t port_num,
                         uint8_t *data,
                         size_t len,
                         uint32_t timeout_ms,
                         size_t *read_len)
{
    if (read_len != NULL) {
        *read_len = 0;
    }
    if (!uart_port_is_ready(port_num)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }

    const int ret = uart_read_bytes(port_num,
                                    data,
                                    (uint32_t)len,
                                    pdMS_TO_TICKS(timeout_ms));
    if (ret < 0) {
        return ESP_FAIL;
    }
    if (read_len != NULL) {
        *read_len = (size_t)ret;
    }
    return ESP_OK;
}

esp_err_t uart_port_get_rx_buffered(uart_port_t port_num, size_t *buffered)
{
    if (buffered == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *buffered = 0;
    if (!uart_port_is_ready(port_num)) {
        return ESP_ERR_INVALID_STATE;
    }

    return uart_get_buffered_data_len(port_num, buffered);
}

bool uart_port_get_config(uart_port_t port_num, uart_port_config_t *config)
{
    if (!uart_port_is_ready(port_num) || config == NULL) {
        return false;
    }
    *config = active_configs[port_num];
    return true;
}
