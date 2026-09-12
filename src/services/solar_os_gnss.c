#include "solar_os_gnss.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "solar_os_expansion.h"
#include "solar_os_uart.h"
#include "uart_port.h"

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char uart_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uart_port_t port;
} solar_os_gnss_device_t;

static const char *TAG = "uart-gnss";
static solar_os_gnss_device_t gnss;

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *uart_bus,
                                size_t uart_bus_len)
{
    bool have_uart = false;

    if (bindings == NULL || uart_bus == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uart_bus[0] = '\0';

    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *b = &bindings[i];
        if (b->kind == SOLAR_OS_EXPANSION_BINDING_UART_PORT) {
            if (have_uart) {
                return ESP_ERR_INVALID_ARG;
            }
            strlcpy(uart_bus, b->target, uart_bus_len);
            have_uart = true;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return have_uart ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t solar_os_gnss_attach(const char *name,
                               const solar_os_expansion_binding_t *bindings,
                               size_t binding_count)
{
    char uart_bus[SOLAR_OS_EXPANSION_TARGET_MAX];

    if (name == NULL || name[0] == '\0' || gnss.active) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = parse_bindings(bindings, binding_count,
                                   uart_bus, sizeof(uart_bus));
    if (ret != ESP_OK) {
        return ret;
    }

    solar_os_expansion_uart_port_t port;
    if (!solar_os_expansion_find_uart_port(uart_bus, &port, NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    const solar_os_bus_uart_config_t config = {
        .port     = port.port,
        .tx_pin   = port.tx_pin,
        .rx_pin   = port.rx_pin,
        .baud_rate = port.baud_rate,
    };
    ret = solar_os_uart_register_bus(uart_bus, &config, false);
    if (ret != ESP_OK) {
        return ret;
    }

    memset(&gnss, 0, sizeof(gnss));
    gnss.active = true;
    gnss.port   = (uart_port_t)port.port;
    strlcpy(gnss.name, name, sizeof(gnss.name));
    strlcpy(gnss.uart_bus, uart_bus, sizeof(gnss.uart_bus));
    ESP_LOGI(TAG, "%s attached on %s UART%d TX=%d RX=%d baud=%" PRIu32,
             name, uart_bus, port.port, port.tx_pin, port.rx_pin, port.baud_rate);
    return ESP_OK;
}

esp_err_t solar_os_gnss_read_raw(uint8_t *buf, size_t len,
                                 uint32_t timeout_ms, size_t *read_len)
{
    if (!gnss.active || buf == NULL || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return uart_port_read(gnss.port, buf, len, timeout_ms, read_len);
}

esp_err_t solar_os_gnss_write_raw(const uint8_t *buf, size_t len, size_t *written)
{
    if (!gnss.active || buf == NULL || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return uart_port_write(gnss.port, buf, len, written);
}

esp_err_t solar_os_gnss_detach(const char *name)
{
    if (!gnss.active || name == NULL || strcmp(gnss.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t ret = solar_os_uart_unregister_bus(gnss.uart_bus);
    if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
        memset(&gnss, 0, sizeof(gnss));
        return ESP_OK;
    }
    return ret;
}
