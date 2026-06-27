#include "solar_os_uart.h"

#include <inttypes.h>
#include <string.h>

#include "solar_os_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "solar_os_board_caps.h"
#include "solar_os_port.h"

#if SOLAR_OS_BOARD_HAS_UART
#include "uart_port.h"
#include "solar_os_board.h"
#endif

#define UART_NVS_NAMESPACE "uart"
#define UART_NVS_BAUD_KEY "baud"
#define UART_NVS_MODE_KEY "mode"
#define UART_RX_BUFFER_SIZE 4096
#define UART_TX_BUFFER_SIZE 1024

static const char *TAG = "solar_os_uart";

static SemaphoreHandle_t uart_mutex;
static bool uart_initialized;
static uint32_t uart_baud_rate = SOLAR_OS_UART_DEFAULT_BAUD_RATE;
static solar_os_uart_mode_t uart_mode = SOLAR_OS_UART_MODE_RAW;

#if SOLAR_OS_BOARD_HAS_UART
static esp_err_t uart_port_read_cb(void *user,
                                   uint8_t *data,
                                   size_t len,
                                   uint32_t timeout_ms,
                                   size_t *read_len);
static esp_err_t uart_port_write_cb(void *user,
                                    const uint8_t *data,
                                    size_t len,
                                    size_t *written);
#endif

static void uart_lock(void)
{
    if (uart_mutex != NULL) {
        xSemaphoreTake(uart_mutex, portMAX_DELAY);
    }
}

static void uart_unlock(void)
{
    if (uart_mutex != NULL) {
        xSemaphoreGive(uart_mutex);
    }
}

static bool uart_try_lock(void)
{
    return uart_mutex == NULL || xSemaphoreTake(uart_mutex, 0) == pdTRUE;
}

bool solar_os_uart_is_valid_baud_rate(uint32_t baud_rate)
{
    return baud_rate >= SOLAR_OS_UART_MIN_BAUD_RATE &&
           baud_rate <= SOLAR_OS_UART_MAX_BAUD_RATE;
}

const char *solar_os_uart_mode_name(solar_os_uart_mode_t mode)
{
    switch (mode) {
    case SOLAR_OS_UART_MODE_RAW:
        return "raw";
    case SOLAR_OS_UART_MODE_LINE:
        return "line";
    default:
        return "unknown";
    }
}

bool solar_os_uart_parse_mode(const char *text, solar_os_uart_mode_t *mode)
{
    if (text == NULL || mode == NULL) {
        return false;
    }
    if (strcmp(text, "raw") == 0) {
        *mode = SOLAR_OS_UART_MODE_RAW;
        return true;
    }
    if (strcmp(text, "line") == 0) {
        *mode = SOLAR_OS_UART_MODE_LINE;
        return true;
    }
    return false;
}

static void uart_load_config(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(UART_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) {
        return;
    }

    uint32_t baud_rate = 0;
    ret = nvs_get_u32(nvs, UART_NVS_BAUD_KEY, &baud_rate);
    if (ret == ESP_OK && solar_os_uart_is_valid_baud_rate(baud_rate)) {
        uart_baud_rate = baud_rate;
    }

    uint8_t mode = 0;
    ret = nvs_get_u8(nvs, UART_NVS_MODE_KEY, &mode);
    if (ret == ESP_OK && mode <= SOLAR_OS_UART_MODE_LINE) {
        uart_mode = (solar_os_uart_mode_t)mode;
    }

    nvs_close(nvs);
}

static esp_err_t uart_save_config(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(UART_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u32(nvs, UART_NVS_BAUD_KEY, uart_baud_rate);
    if (ret == ESP_OK) {
        ret = nvs_set_u8(nvs, UART_NVS_MODE_KEY, (uint8_t)uart_mode);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return ret;
}

#if SOLAR_OS_BOARD_HAS_UART
static esp_err_t uart_register_stream_port(void)
{
    const solar_os_port_driver_t driver = {
        .name = SOLAR_OS_UART_PORT_NAME,
        .label = "expansion UART",
        .capabilities = SOLAR_OS_PORT_CAP_READ |
            SOLAR_OS_PORT_CAP_WRITE |
            SOLAR_OS_PORT_CAP_CONFIG,
        .read = uart_port_read_cb,
        .write = uart_port_write_cb,
        .user = NULL,
    };

    const esp_err_t ret = solar_os_port_register(&driver);
    return ret == ESP_ERR_INVALID_STATE ? ESP_OK : ret;
}

static esp_err_t uart_require_port_idle(void)
{
    solar_os_port_info_t info;
    const esp_err_t ret = solar_os_port_get_info(SOLAR_OS_UART_PORT_NAME, &info);
    if (ret == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    return info.claimed ? ESP_ERR_INVALID_STATE : ESP_OK;
}
#endif

esp_err_t solar_os_uart_init(void)
{
#if !SOLAR_OS_BOARD_HAS_UART
    uart_load_config();
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (uart_initialized) {
        return ESP_OK;
    }

    if (uart_mutex == NULL) {
        uart_mutex = xSemaphoreCreateMutex();
        if (uart_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    uart_lock();
    if (uart_initialized) {
        uart_unlock();
        return ESP_OK;
    }

    uart_load_config();

    const uart_port_config_t config = {
        .port_num = SOLAR_OS_BOARD_UART_PORT,
        .tx_pin = SOLAR_OS_BOARD_PIN_UART_TX,
        .rx_pin = SOLAR_OS_BOARD_PIN_UART_RX,
        .baud_rate = uart_baud_rate,
        .rx_buffer_size = UART_RX_BUFFER_SIZE,
        .tx_buffer_size = UART_TX_BUFFER_SIZE,
    };
    const esp_err_t ret = uart_port_init(&config);
    if (ret == ESP_OK) {
        const esp_err_t port_ret = uart_register_stream_port();
        if (port_ret == ESP_OK) {
            uart_initialized = true;
            SOLAR_OS_LOGI(TAG,
                     "UART service ready: UART%d TX=%d RX=%d baud=%" PRIu32 " mode=%s",
                     (int)SOLAR_OS_BOARD_UART_PORT,
                     (int)SOLAR_OS_BOARD_PIN_UART_TX,
                     (int)SOLAR_OS_BOARD_PIN_UART_RX,
                     uart_baud_rate,
                     solar_os_uart_mode_name(uart_mode));
        } else {
            uart_unlock();
            return port_ret;
        }
    }

    uart_unlock();
    return ret;
#endif
}

esp_err_t solar_os_uart_set_baud_rate(uint32_t baud_rate)
{
    if (!solar_os_uart_is_valid_baud_rate(baud_rate)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = solar_os_uart_init();
    if (ret != ESP_OK) {
        return ret;
    }

#if !SOLAR_OS_BOARD_HAS_UART
    return ESP_ERR_NOT_SUPPORTED;
#else
    ret = uart_require_port_idle();
    if (ret != ESP_OK) {
        return ret;
    }

    uart_lock();
    ret = uart_port_set_baud_rate(baud_rate);
    if (ret == ESP_OK) {
        uart_baud_rate = baud_rate;
    }
    uart_unlock();

    if (ret != ESP_OK) {
        return ret;
    }
    return uart_save_config();
#endif
}

esp_err_t solar_os_uart_set_mode(solar_os_uart_mode_t mode)
{
    if (mode != SOLAR_OS_UART_MODE_RAW && mode != SOLAR_OS_UART_MODE_LINE) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = solar_os_uart_init();
    if (ret != ESP_OK) {
        return ret;
    }

#if !SOLAR_OS_BOARD_HAS_UART
    return ESP_ERR_NOT_SUPPORTED;
#else
    ret = uart_require_port_idle();
    if (ret != ESP_OK) {
        return ret;
    }

    uart_lock();
    uart_mode = mode;
    uart_unlock();
    return uart_save_config();
#endif
}

#if SOLAR_OS_BOARD_HAS_UART
static esp_err_t uart_write_direct(const uint8_t *data, size_t len, size_t *written)
{
    if (written != NULL) {
        *written = 0;
    }

    uart_lock();
    const esp_err_t ret = uart_port_write(data, len, written);
    uart_unlock();
    return ret;
}
#endif

esp_err_t solar_os_uart_write(const uint8_t *data, size_t len, size_t *written)
{
    if (written != NULL) {
        *written = 0;
    }
    if (data == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }

#if !SOLAR_OS_BOARD_HAS_UART
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_err_t ret = solar_os_uart_init();
    if (ret != ESP_OK) {
        return ret;
    }

    solar_os_port_handle_t handle = SOLAR_OS_PORT_HANDLE_INIT;
    ret = solar_os_port_claim(SOLAR_OS_UART_PORT_NAME, "uart", &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = solar_os_port_write(&handle, data, len, written);
    const esp_err_t release_ret = solar_os_port_release(&handle);
    return ret == ESP_OK ? release_ret : ret;
#endif
}

#if SOLAR_OS_BOARD_HAS_UART
static esp_err_t uart_read_line_mode(uint8_t *data,
                                     size_t len,
                                     uint32_t timeout_ms,
                                     size_t *read_len)
{
    size_t total = 0;
    const int64_t deadline_us = esp_timer_get_time() + ((int64_t)timeout_ms * 1000);

    while (total < len) {
        int64_t remaining_us = deadline_us - esp_timer_get_time();
        uint32_t remaining_ms = 0;
        if (timeout_ms > 0) {
            if (remaining_us <= 0) {
                break;
            }
            remaining_ms = (uint32_t)((remaining_us + 999) / 1000);
        }

        size_t got = 0;
        const esp_err_t ret = uart_port_read(&data[total], 1, remaining_ms, &got);
        if (ret != ESP_OK) {
            return ret;
        }
        if (got == 0) {
            break;
        }

        total++;
        if (data[total - 1] == '\n') {
            break;
        }
    }

    if (read_len != NULL) {
        *read_len = total;
    }
    return ESP_OK;
}

static esp_err_t uart_read_direct(uint8_t *data,
                                  size_t len,
                                  uint32_t timeout_ms,
                                  size_t *read_len)
{
    if (read_len != NULL) {
        *read_len = 0;
    }
    if (data == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }

    uart_lock();
    esp_err_t ret = ESP_OK;
    if (uart_mode == SOLAR_OS_UART_MODE_LINE) {
        ret = uart_read_line_mode(data, len, timeout_ms, read_len);
    } else {
        ret = uart_port_read(data, len, timeout_ms, read_len);
    }
    uart_unlock();
    return ret;
}

static esp_err_t uart_port_read_cb(void *user,
                                   uint8_t *data,
                                   size_t len,
                                   uint32_t timeout_ms,
                                   size_t *read_len)
{
    (void)user;
    return uart_read_direct(data, len, timeout_ms, read_len);
}

static esp_err_t uart_port_write_cb(void *user,
                                    const uint8_t *data,
                                    size_t len,
                                    size_t *written)
{
    (void)user;
    return uart_write_direct(data, len, written);
}
#endif

esp_err_t solar_os_uart_read(uint8_t *data, size_t len, uint32_t timeout_ms, size_t *read_len)
{
    if (read_len != NULL) {
        *read_len = 0;
    }
    if (data == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }

#if !SOLAR_OS_BOARD_HAS_UART
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_err_t ret = solar_os_uart_init();
    if (ret != ESP_OK) {
        return ret;
    }

    solar_os_port_handle_t handle = SOLAR_OS_PORT_HANDLE_INIT;
    ret = solar_os_port_claim(SOLAR_OS_UART_PORT_NAME, "uart", &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = solar_os_port_read(&handle, data, len, timeout_ms, read_len);
    const esp_err_t release_ret = solar_os_port_release(&handle);
    return ret == ESP_OK ? release_ret : ret;
#endif
}

void solar_os_uart_get_status(solar_os_uart_status_t *status)
{
    if (status == NULL) {
        return;
    }

    const bool locked = uart_try_lock();
    *status = (solar_os_uart_status_t){
        .initialized = uart_initialized,
#if SOLAR_OS_BOARD_HAS_UART
        .port_num = (int)SOLAR_OS_BOARD_UART_PORT,
        .tx_pin = (int)SOLAR_OS_BOARD_PIN_UART_TX,
        .rx_pin = (int)SOLAR_OS_BOARD_PIN_UART_RX,
#else
        .port_num = -1,
        .tx_pin = -1,
        .rx_pin = -1,
#endif
        .baud_rate = uart_baud_rate,
        .mode = uart_mode,
        .rx_buffered = 0,
        .rx_buffered_valid = false,
    };
#if SOLAR_OS_BOARD_HAS_UART
    if (locked && uart_initialized) {
        size_t buffered = 0;
        if (uart_port_get_rx_buffered(&buffered) == ESP_OK) {
            status->rx_buffered = buffered;
            status->rx_buffered_valid = true;
        }
    }
    if (locked) {
        uart_unlock();
    }

    solar_os_port_info_t port_info;
    if (solar_os_port_get_info(SOLAR_OS_UART_PORT_NAME, &port_info) == ESP_OK) {
        status->port_claimed = port_info.claimed;
        if (port_info.claimed) {
            strlcpy(status->port_owner, port_info.owner, sizeof(status->port_owner));
        }
    }
#endif
}
