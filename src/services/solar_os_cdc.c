#include "solar_os_cdc.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "solar_os_board_caps.h"
#if SOLAR_OS_BOARD_HAS_CDC
#include "cdc_port.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_log.h"

#define CDC_RX_BUFFER_SIZE 4096
#define CDC_TX_BUFFER_SIZE 2048

static const char *TAG = "solar_os_cdc";

static SemaphoreHandle_t cdc_mutex;
static bool cdc_initialized;

static void cdc_lock(void)
{
    if (cdc_mutex != NULL) {
        xSemaphoreTake(cdc_mutex, portMAX_DELAY);
    }
}

static void cdc_unlock(void)
{
    if (cdc_mutex != NULL) {
        xSemaphoreGive(cdc_mutex);
    }
}

#if SOLAR_OS_BOARD_HAS_CDC
static esp_err_t cdc_port_read_cb(void *user,
                                  uint8_t *data,
                                  size_t len,
                                  uint32_t timeout_ms,
                                  size_t *read_len)
{
    (void)user;
    return cdc_port_read(data, len, timeout_ms, read_len);
}

static esp_err_t cdc_port_write_cb(void *user,
                                   const uint8_t *data,
                                   size_t len,
                                   size_t *written)
{
    (void)user;
    return cdc_port_write(data, len, written);
}

static esp_err_t cdc_register_stream_port(void)
{
    const solar_os_port_driver_t driver = {
        .name = SOLAR_OS_CDC_PORT_NAME,
        .label = "USB CDC console",
        .capabilities = SOLAR_OS_PORT_CAP_READ | SOLAR_OS_PORT_CAP_WRITE,
        .read = cdc_port_read_cb,
        .write = cdc_port_write_cb,
        .user = NULL,
    };

    const esp_err_t ret = solar_os_port_register(&driver);
    return ret == ESP_ERR_INVALID_STATE ? ESP_OK : ret;
}
#endif

esp_err_t solar_os_cdc_init(void)
{
#if !SOLAR_OS_BOARD_HAS_CDC
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (cdc_initialized) {
        return ESP_OK;
    }

    if (cdc_mutex == NULL) {
        cdc_mutex = xSemaphoreCreateMutex();
        if (cdc_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    cdc_lock();
    if (cdc_initialized) {
        cdc_unlock();
        return ESP_OK;
    }

    const cdc_port_config_t config = {
        .rx_buffer_size = CDC_RX_BUFFER_SIZE,
        .tx_buffer_size = CDC_TX_BUFFER_SIZE,
    };

    esp_err_t ret = cdc_port_init(&config);
    if (ret == ESP_OK) {
        ret = cdc_register_stream_port();
    }
    if (ret == ESP_OK) {
        cdc_initialized = true;
        SOLAR_OS_LOGI(TAG,
                      "CDC port ready: %s",
                      cdc_port_is_connected() ? "connected" : "not connected");
    }

    cdc_unlock();
    return ret;
#endif
}

void solar_os_cdc_get_status(solar_os_cdc_status_t *status)
{
    if (status == NULL) {
        return;
    }

    cdc_lock();
    *status = (solar_os_cdc_status_t){
        .initialized = cdc_initialized,
#if SOLAR_OS_BOARD_HAS_CDC
        .connected = cdc_port_is_connected(),
        .driver_installed = cdc_port_driver_installed(),
#else
        .connected = false,
        .driver_installed = false,
#endif
    };
    cdc_unlock();

    solar_os_port_info_t port_info;
    if (solar_os_port_get_info(SOLAR_OS_CDC_PORT_NAME, &port_info) == ESP_OK) {
        status->port_claimed = port_info.claimed;
        if (port_info.claimed) {
            strlcpy(status->port_owner, port_info.owner, sizeof(status->port_owner));
        }
    }
}
