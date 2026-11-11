#include "cdc_port.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/usb_serial_jtag.h"
#include "esp_ipc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static bool ready;
static int driver_core;

esp_err_t cdc_port_init(const cdc_port_config_t *config)
{
    if (config == NULL || config->rx_buffer_size == 0 || config->tx_buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t driver_config = {
            .tx_buffer_size = (uint32_t)config->tx_buffer_size,
            .rx_buffer_size = (uint32_t)config->rx_buffer_size,
        };
        const esp_err_t ret = usb_serial_jtag_driver_install(&driver_config);
        if (ret != ESP_OK) {
            ready = usb_serial_jtag_is_driver_installed();
            return ready ? ESP_OK : ret;
        }
        driver_core = xPortGetCoreID();
    }

    ready = true;
    return ESP_OK;
}

static void uninstall_on_driver_core(void *arg)
{
    *(esp_err_t *)arg = usb_serial_jtag_driver_uninstall();
}

esp_err_t cdc_port_deinit(void)
{
    if (!usb_serial_jtag_is_driver_installed()) {
        ready = false;
        return ESP_OK;
    }
    esp_err_t ret;
    if (xPortGetCoreID() == driver_core) {
        ret = usb_serial_jtag_driver_uninstall();
    } else {
        const esp_err_t ipc_err = esp_ipc_call_blocking(driver_core,
            uninstall_on_driver_core, &ret);
        if (ipc_err != ESP_OK) return ipc_err;
    }
    if (ret == ESP_OK) ready = false;
    return ret;
}

bool cdc_port_is_ready(void)
{
    return ready;
}

bool cdc_port_is_connected(void)
{
    return ready && usb_serial_jtag_is_connected();
}

bool cdc_port_driver_installed(void)
{
    return usb_serial_jtag_is_driver_installed();
}

esp_err_t cdc_port_write(const uint8_t *data, size_t len, size_t *written)
{
    if (written != NULL) {
        *written = 0;
    }
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }

    const int ret = usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(100));
    if (ret < 0) {
        return ESP_FAIL;
    }
    if (written != NULL) {
        *written = (size_t)ret;
    }
    return (size_t)ret == len ? ESP_OK : ESP_FAIL;
}

esp_err_t cdc_port_read(uint8_t *data, size_t len, uint32_t timeout_ms, size_t *read_len)
{
    if (read_len != NULL) {
        *read_len = 0;
    }
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }

    const int ret = usb_serial_jtag_read_bytes(data, (uint32_t)len, pdMS_TO_TICKS(timeout_ms));
    if (ret < 0) {
        return ESP_FAIL;
    }
    if (read_len != NULL) {
        *read_len = (size_t)ret;
    }
    return ESP_OK;
}
