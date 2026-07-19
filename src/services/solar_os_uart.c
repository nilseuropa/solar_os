#include "solar_os_uart.h"

#include <inttypes.h>
#include <string.h>

#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "solar_os_board.h"
#include "solar_os_board_caps.h"
#include "solar_os_config.h"
#include "solar_os_log.h"
#include "solar_os_port.h"
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
#include "solar_os_buses.h"
#include "solar_os_resources.h"
#endif

#if SOLAR_OS_BOARD_HAS_UART
#include "uart_port.h"
#endif

#define UART_NVS_NAMESPACE "uart"
#define UART_NVS_BAUD_KEY "baud"
#define UART_NVS_MODE_KEY "mode"
#define UART_RX_BUFFER_SIZE 4096
#define UART_TX_BUFFER_SIZE 1024

typedef struct {
    bool active;
    bool attached;
    bool initialized;
    bool persistent;
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    bool resources_claimed;
#endif
    char name[SOLAR_OS_BUS_NAME_MAX];
    solar_os_bus_uart_config_t config;
    solar_os_uart_mode_t mode;
    SemaphoreHandle_t mutex;
} solar_os_uart_instance_t;

static const char *TAG = "solar_os_uart";
static solar_os_uart_instance_t uart_instances[UART_NUM_MAX];
static SemaphoreHandle_t uart_manager_mutex;

static esp_err_t uart_port_read_cb(void *user,
                                   uint8_t *data,
                                   size_t len,
                                   uint32_t timeout_ms,
                                   size_t *read_len);
static esp_err_t uart_port_write_cb(void *user,
                                    const uint8_t *data,
                                    size_t len,
                                    size_t *written);
static esp_err_t uart_port_open_cb(void *user);
static esp_err_t uart_port_close_cb(void *user);

static esp_err_t uart_register_port(solar_os_uart_instance_t *instance)
{
    const solar_os_port_driver_t driver = {
        .name = instance->name,
        .label = instance->persistent ? "board UART" : "runtime UART",
        .capabilities = SOLAR_OS_PORT_CAP_READ |
            SOLAR_OS_PORT_CAP_WRITE |
            SOLAR_OS_PORT_CAP_CONFIG,
        .read = uart_port_read_cb,
        .write = uart_port_write_cb,
        .open = uart_port_open_cb,
        .close = uart_port_close_cb,
        .user = instance,
    };
    return solar_os_port_register(&driver);
}

#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
static void uart_resource_owner(const solar_os_uart_instance_t *instance,
                                char *owner,
                                size_t owner_size)
{
    if (owner == NULL || owner_size == 0) {
        return;
    }
    owner[0] = '\0';
    if (instance != NULL) {
        strlcpy(owner, "bus:", owner_size);
        strlcat(owner, instance->name, owner_size);
    }
}

static esp_err_t uart_claim_resources(solar_os_uart_instance_t *instance)
{
    if (instance == NULL || instance->resources_claimed) {
        return ESP_OK;
    }

    char owner[SOLAR_OS_RESOURCE_OWNER_MAX];
    uart_resource_owner(instance, owner, sizeof(owner));
    const solar_os_resource_request_t requests[] = {
        {
            .kind = SOLAR_OS_RESOURCE_UART_PORT,
            .primary = instance->config.port,
            .secondary = -1,
            .label = "uart-port",
        },
        {
            .kind = SOLAR_OS_RESOURCE_GPIO_PIN,
            .primary = instance->config.tx_pin,
            .secondary = -1,
            .label = "uart-tx",
        },
        {
            .kind = SOLAR_OS_RESOURCE_GPIO_PIN,
            .primary = instance->config.rx_pin,
            .secondary = -1,
            .label = "uart-rx",
        },
    };
    const esp_err_t ret = solar_os_resource_claim_bundle(
        requests,
        sizeof(requests) / sizeof(requests[0]),
        owner,
        NULL);
    if (ret == ESP_OK) {
        instance->resources_claimed = true;
    }
    return ret;
}

static void uart_release_resources(solar_os_uart_instance_t *instance)
{
    if (instance == NULL || !instance->resources_claimed) {
        return;
    }

    char owner[SOLAR_OS_RESOURCE_OWNER_MAX];
    uart_resource_owner(instance, owner, sizeof(owner));
    (void)solar_os_resource_release_owner(owner);
    instance->resources_claimed = false;
}
#endif

static esp_err_t uart_stop_instance_locked(solar_os_uart_instance_t *instance)
{
    esp_err_t ret = ESP_OK;
    if (instance->initialized) {
        ret = uart_port_deinit((uart_port_t)instance->config.port);
        if (ret == ESP_OK) {
            instance->initialized = false;
        }
    }
    return ret;
}

static esp_err_t uart_manager_init(void)
{
    if (uart_manager_mutex == NULL) {
        uart_manager_mutex = xSemaphoreCreateMutex();
    }
    return uart_manager_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static solar_os_uart_instance_t *uart_find_name_locked(const char *name)
{
    for (size_t i = 0; i < UART_NUM_MAX; i++) {
        if (uart_instances[i].active && strcmp(uart_instances[i].name, name) == 0) {
            return &uart_instances[i];
        }
    }
    return NULL;
}

static solar_os_uart_instance_t *uart_find_port_locked(int port_num)
{
    for (size_t i = 0; i < UART_NUM_MAX; i++) {
        if (uart_instances[i].active && uart_instances[i].config.port == port_num) {
            return &uart_instances[i];
        }
    }
    return NULL;
}

static solar_os_uart_instance_t *uart_find_name(const char *name)
{
    if (name == NULL || uart_manager_init() != ESP_OK) {
        return NULL;
    }
    xSemaphoreTake(uart_manager_mutex, portMAX_DELAY);
    solar_os_uart_instance_t *instance = uart_find_name_locked(name);
    xSemaphoreGive(uart_manager_mutex);
    return instance;
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

static void uart_load_config(uint32_t *baud_rate, solar_os_uart_mode_t *mode)
{
    nvs_handle_t nvs;
    if (nvs_open(UART_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    uint32_t saved_baud = 0;
    if (nvs_get_u32(nvs, UART_NVS_BAUD_KEY, &saved_baud) == ESP_OK &&
        solar_os_uart_is_valid_baud_rate(saved_baud)) {
        *baud_rate = saved_baud;
    }

    uint8_t saved_mode = 0;
    if (nvs_get_u8(nvs, UART_NVS_MODE_KEY, &saved_mode) == ESP_OK &&
        saved_mode <= SOLAR_OS_UART_MODE_LINE) {
        *mode = (solar_os_uart_mode_t)saved_mode;
    }
    nvs_close(nvs);
}

static esp_err_t uart_save_config(const solar_os_uart_instance_t *instance)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(UART_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_u32(nvs, UART_NVS_BAUD_KEY, instance->config.baud_rate);
    if (ret == ESP_OK) {
        ret = nvs_set_u8(nvs, UART_NVS_MODE_KEY, (uint8_t)instance->mode);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

esp_err_t solar_os_uart_register_bus(const char *name,
                                     const solar_os_bus_uart_config_t *config,
                                     bool persistent)
{
#if !SOLAR_OS_BOARD_HAS_UART
    (void)name;
    (void)config;
    (void)persistent;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (name == NULL || name[0] == '\0' ||
        strnlen(name, SOLAR_OS_BUS_NAME_MAX) >= SOLAR_OS_BUS_NAME_MAX ||
        config == NULL || config->port < 0 || config->port >= UART_NUM_MAX ||
        config->tx_pin < 0 || config->rx_pin < 0 ||
        config->tx_pin == config->rx_pin ||
        !solar_os_uart_is_valid_baud_rate(config->baud_rate)) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(uart_manager_init(), TAG, "create UART manager mutex failed");

    xSemaphoreTake(uart_manager_mutex, portMAX_DELAY);
    solar_os_uart_instance_t *existing = uart_find_name_locked(name);
    if (existing != NULL) {
        const bool same = existing->config.port == config->port &&
            existing->config.tx_pin == config->tx_pin &&
            existing->config.rx_pin == config->rx_pin;
        xSemaphoreGive(uart_manager_mutex);
        return same ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (uart_find_port_locked(config->port) != NULL) {
        xSemaphoreGive(uart_manager_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    solar_os_uart_instance_t *instance = NULL;
    for (size_t i = 0; i < UART_NUM_MAX; i++) {
        if (!uart_instances[i].active) {
            instance = &uart_instances[i];
            break;
        }
    }
    if (instance == NULL) {
        xSemaphoreGive(uart_manager_mutex);
        return ESP_ERR_NO_MEM;
    }

    SemaphoreHandle_t instance_mutex = xSemaphoreCreateMutex();
    if (instance_mutex == NULL) {
        xSemaphoreGive(uart_manager_mutex);
        return ESP_ERR_NO_MEM;
    }
    memset(instance, 0, sizeof(*instance));
    instance->active = true;
    instance->persistent = persistent;
    instance->config = *config;
    instance->mode = SOLAR_OS_UART_MODE_RAW;
    instance->mutex = instance_mutex;
    strlcpy(instance->name, name, sizeof(instance->name));

    esp_err_t ret = ESP_OK;
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    ret = uart_claim_resources(instance);
#endif
    if (ret == ESP_OK) {
        ret = uart_register_port(instance);
    }
    if (ret != ESP_OK) {
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
        uart_release_resources(instance);
#endif
        vSemaphoreDelete(instance_mutex);
        memset(instance, 0, sizeof(*instance));
        xSemaphoreGive(uart_manager_mutex);
        return ret;
    }
    instance->attached = true;

    SOLAR_OS_LOGI(TAG,
                  "%s registered: UART%d TX=%d RX=%d baud=%" PRIu32 " mode=%s",
                  name,
                  config->port,
                  config->tx_pin,
                  config->rx_pin,
                  config->baud_rate,
                  solar_os_uart_mode_name(instance->mode));
    xSemaphoreGive(uart_manager_mutex);
    return ESP_OK;
#endif
}

esp_err_t solar_os_uart_start_bus(const char *name)
{
#if !SOLAR_OS_BOARD_HAS_UART
    (void)name;
    return ESP_ERR_NOT_SUPPORTED;
#else
    solar_os_uart_instance_t *instance = uart_find_name(name);
    if (instance == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    xSemaphoreTake(instance->mutex, portMAX_DELAY);
    if (!instance->attached) {
        xSemaphoreGive(instance->mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (instance->initialized) {
        xSemaphoreGive(instance->mutex);
        return ESP_OK;
    }
    esp_err_t ret = ESP_OK;
    const uart_port_config_t driver_config = {
        .port_num = (uart_port_t)instance->config.port,
        .tx_pin = (gpio_num_t)instance->config.tx_pin,
        .rx_pin = (gpio_num_t)instance->config.rx_pin,
        .baud_rate = instance->config.baud_rate,
        .rx_buffer_size = UART_RX_BUFFER_SIZE,
        .tx_buffer_size = UART_TX_BUFFER_SIZE,
    };
    ret = uart_port_init(&driver_config);
    if (ret == ESP_OK) {
        instance->initialized = true;
        SOLAR_OS_LOGI(TAG,
                      "%s ready: UART%d TX=%d RX=%d baud=%" PRIu32,
                      instance->name,
                      instance->config.port,
                      instance->config.tx_pin,
                      instance->config.rx_pin,
                      instance->config.baud_rate);
    }
    xSemaphoreGive(instance->mutex);
    return ret;
#endif
}

esp_err_t solar_os_uart_stop_bus(const char *name)
{
#if !SOLAR_OS_BOARD_HAS_UART
    (void)name;
    return ESP_ERR_NOT_SUPPORTED;
#else
    solar_os_uart_instance_t *instance = uart_find_name(name);
    if (instance == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    solar_os_port_info_t info;
    if (solar_os_port_get_info(name, &info) == ESP_OK && info.claimed) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(instance->mutex, portMAX_DELAY);
    const esp_err_t ret = uart_stop_instance_locked(instance);
    xSemaphoreGive(instance->mutex);
    return ret;
#endif
}

esp_err_t solar_os_uart_unregister_bus(const char *name)
{
#if !SOLAR_OS_BOARD_HAS_UART
    (void)name;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (name == NULL || uart_manager_init() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(uart_manager_mutex, portMAX_DELAY);
    solar_os_uart_instance_t *instance = uart_find_name_locked(name);
    if (instance == NULL) {
        xSemaphoreGive(uart_manager_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    if (instance->persistent) {
        xSemaphoreGive(uart_manager_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    solar_os_port_info_t port_info;
    if (solar_os_port_get_info(name, &port_info) == ESP_OK && port_info.claimed) {
        xSemaphoreGive(uart_manager_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = uart_stop_instance_locked(instance);
    if (ret == ESP_OK && instance->attached) {
        ret = solar_os_port_unregister(name);
    }
    if (ret == ESP_OK) {
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
        uart_release_resources(instance);
#endif
        SemaphoreHandle_t instance_mutex = instance->mutex;
        memset(instance, 0, sizeof(*instance));
        vSemaphoreDelete(instance_mutex);
    }
    xSemaphoreGive(uart_manager_mutex);
    return ret;
#endif
}

esp_err_t solar_os_uart_bus_attach(const char *name)
{
#if !SOLAR_OS_BOARD_HAS_UART
    (void)name;
    return ESP_ERR_NOT_SUPPORTED;
#else
    solar_os_uart_instance_t *instance = uart_find_name(name);
    if (instance == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    xSemaphoreTake(instance->mutex, portMAX_DELAY);
    if (instance->attached) {
        xSemaphoreGive(instance->mutex);
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    ret = uart_claim_resources(instance);
#endif
    if (ret == ESP_OK) {
        ret = uart_register_port(instance);
    }
    if (ret == ESP_OK) {
        instance->attached = true;
        SOLAR_OS_LOGI(TAG, "%s attached", instance->name);
    } else {
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
        uart_release_resources(instance);
#endif
    }
    xSemaphoreGive(instance->mutex);
    return ret;
#endif
}

esp_err_t solar_os_uart_bus_detach(const char *name)
{
#if !SOLAR_OS_BOARD_HAS_UART
    (void)name;
    return ESP_ERR_NOT_SUPPORTED;
#else
    solar_os_uart_instance_t *instance = uart_find_name(name);
    if (instance == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    xSemaphoreTake(instance->mutex, portMAX_DELAY);
    if (!instance->attached) {
        xSemaphoreGive(instance->mutex);
        return ESP_OK;
    }

    esp_err_t ret = solar_os_port_unregister(instance->name);
    if (ret == ESP_OK) {
        ret = uart_stop_instance_locked(instance);
        if (ret != ESP_OK) {
            (void)uart_register_port(instance);
        }
    }
    if (ret == ESP_OK) {
        instance->attached = false;
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
        uart_release_resources(instance);
#endif
        SOLAR_OS_LOGI(TAG, "%s detached", instance->name);
    }
    xSemaphoreGive(instance->mutex);
    return ret;
#endif
}

esp_err_t solar_os_uart_attach(void)
{
    ESP_RETURN_ON_ERROR(solar_os_uart_init(), TAG, "initialize default UART failed");
    return solar_os_uart_bus_attach(SOLAR_OS_UART_PORT_NAME);
}

esp_err_t solar_os_uart_detach(void)
{
    ESP_RETURN_ON_ERROR(solar_os_uart_init(), TAG, "initialize default UART failed");
    return solar_os_uart_bus_detach(SOLAR_OS_UART_PORT_NAME);
}

esp_err_t solar_os_uart_init(void)
{
#if !SOLAR_OS_BOARD_HAS_UART
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (uart_find_name(SOLAR_OS_UART_PORT_NAME) != NULL) {
        return ESP_OK;
    }

    solar_os_bus_uart_config_t config = {
        .port = SOLAR_OS_BOARD_UART_PORT,
        .tx_pin = SOLAR_OS_BOARD_PIN_UART_TX,
        .rx_pin = SOLAR_OS_BOARD_PIN_UART_RX,
        .baud_rate = SOLAR_OS_UART_DEFAULT_BAUD_RATE,
    };
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    solar_os_bus_info_t info;
    if (solar_os_bus_find(SOLAR_OS_UART_PORT_NAME,
                          SOLAR_OS_BUS_PROTOCOL_UART,
                          &info)) {
        config = info.config.uart;
    }
#endif
    solar_os_uart_mode_t mode = SOLAR_OS_UART_MODE_RAW;
    uart_load_config(&config.baud_rate, &mode);
    esp_err_t ret = solar_os_uart_register_bus(SOLAR_OS_UART_PORT_NAME, &config, true);
    if (ret == ESP_OK) {
        solar_os_uart_instance_t *instance = uart_find_name(SOLAR_OS_UART_PORT_NAME);
        if (instance != NULL) {
            xSemaphoreTake(instance->mutex, portMAX_DELAY);
            instance->mode = mode;
            xSemaphoreGive(instance->mutex);
        }
#if !SOLAR_OS_PACKAGE_SERVICE_RESOURCES
        ret = solar_os_uart_start_bus(SOLAR_OS_UART_PORT_NAME);
#endif
    }
    return ret;
#endif
}

static esp_err_t uart_require_idle(const char *name)
{
    solar_os_port_info_t info;
    const esp_err_t ret = solar_os_port_get_info(name, &info);
    if (ret == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    return info.claimed ? ESP_ERR_INVALID_STATE : ESP_OK;
}

esp_err_t solar_os_uart_bus_set_baud_rate(const char *name, uint32_t baud_rate)
{
    if (name == NULL || !solar_os_uart_is_valid_baud_rate(baud_rate)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(name, SOLAR_OS_UART_PORT_NAME) == 0) {
        ESP_RETURN_ON_ERROR(solar_os_uart_init(), TAG, "initialize default UART failed");
    }
    solar_os_uart_instance_t *instance = uart_find_name(name);
    if (instance == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(uart_require_idle(name), TAG, "UART port busy");

    xSemaphoreTake(instance->mutex, portMAX_DELAY);
    esp_err_t ret = ESP_OK;
    if (instance->initialized) {
        ret = uart_port_set_baud_rate((uart_port_t)instance->config.port, baud_rate);
    }
    if (ret == ESP_OK) {
        instance->config.baud_rate = baud_rate;
        if (instance->persistent) {
            ret = uart_save_config(instance);
        }
    }
    xSemaphoreGive(instance->mutex);
    return ret;
}

esp_err_t solar_os_uart_set_baud_rate(uint32_t baud_rate)
{
    return solar_os_uart_bus_set_baud_rate(SOLAR_OS_UART_PORT_NAME, baud_rate);
}

esp_err_t solar_os_uart_bus_set_mode(const char *name, solar_os_uart_mode_t mode)
{
    if (name == NULL ||
        (mode != SOLAR_OS_UART_MODE_RAW && mode != SOLAR_OS_UART_MODE_LINE)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(name, SOLAR_OS_UART_PORT_NAME) == 0) {
        ESP_RETURN_ON_ERROR(solar_os_uart_init(), TAG, "initialize default UART failed");
    }
    solar_os_uart_instance_t *instance = uart_find_name(name);
    if (instance == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(uart_require_idle(name), TAG, "UART port busy");

    xSemaphoreTake(instance->mutex, portMAX_DELAY);
    instance->mode = mode;
    const esp_err_t ret = instance->persistent ? uart_save_config(instance) : ESP_OK;
    xSemaphoreGive(instance->mutex);
    return ret;
}

esp_err_t solar_os_uart_set_mode(solar_os_uart_mode_t mode)
{
    return solar_os_uart_bus_set_mode(SOLAR_OS_UART_PORT_NAME, mode);
}

static esp_err_t uart_write_direct(solar_os_uart_instance_t *instance,
                                   const uint8_t *data,
                                   size_t len,
                                   size_t *written)
{
    ESP_RETURN_ON_ERROR(solar_os_uart_start_bus(instance->name),
                        TAG,
                        "start UART bus failed");
    xSemaphoreTake(instance->mutex, portMAX_DELAY);
    const esp_err_t ret = uart_port_write((uart_port_t)instance->config.port,
                                          data,
                                          len,
                                          written);
    xSemaphoreGive(instance->mutex);
    return ret;
}

static esp_err_t uart_read_line_mode(solar_os_uart_instance_t *instance,
                                     uint8_t *data,
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
        const esp_err_t ret = uart_port_read((uart_port_t)instance->config.port,
                                             &data[total],
                                             1,
                                             remaining_ms,
                                             &got);
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

static esp_err_t uart_read_direct(solar_os_uart_instance_t *instance,
                                  uint8_t *data,
                                  size_t len,
                                  uint32_t timeout_ms,
                                  size_t *read_len)
{
    ESP_RETURN_ON_ERROR(solar_os_uart_start_bus(instance->name),
                        TAG,
                        "start UART bus failed");
    xSemaphoreTake(instance->mutex, portMAX_DELAY);
    const esp_err_t ret = instance->mode == SOLAR_OS_UART_MODE_LINE
        ? uart_read_line_mode(instance, data, len, timeout_ms, read_len)
        : uart_port_read((uart_port_t)instance->config.port,
                         data,
                         len,
                         timeout_ms,
                         read_len);
    xSemaphoreGive(instance->mutex);
    return ret;
}

static esp_err_t uart_port_read_cb(void *user,
                                   uint8_t *data,
                                   size_t len,
                                   uint32_t timeout_ms,
                                   size_t *read_len)
{
    return user != NULL
        ? uart_read_direct((solar_os_uart_instance_t *)user,
                           data,
                           len,
                           timeout_ms,
                           read_len)
        : ESP_ERR_INVALID_ARG;
}

static esp_err_t uart_port_write_cb(void *user,
                                    const uint8_t *data,
                                    size_t len,
                                    size_t *written)
{
    return user != NULL
        ? uart_write_direct((solar_os_uart_instance_t *)user, data, len, written)
        : ESP_ERR_INVALID_ARG;
}

static esp_err_t uart_port_open_cb(void *user)
{
    solar_os_uart_instance_t *instance = (solar_os_uart_instance_t *)user;
    return instance != NULL ? solar_os_uart_start_bus(instance->name) : ESP_ERR_INVALID_ARG;
}

static esp_err_t uart_port_close_cb(void *user)
{
    solar_os_uart_instance_t *instance = (solar_os_uart_instance_t *)user;
    if (instance == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(instance->mutex, portMAX_DELAY);
    const esp_err_t ret = uart_stop_instance_locked(instance);
    xSemaphoreGive(instance->mutex);
    return ret;
}

esp_err_t solar_os_uart_bus_write(const char *name,
                                  const uint8_t *data,
                                  size_t len,
                                  size_t *written)
{
    if (written != NULL) {
        *written = 0;
    }
    if (name == NULL || (data == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }
    if (strcmp(name, SOLAR_OS_UART_PORT_NAME) == 0) {
        ESP_RETURN_ON_ERROR(solar_os_uart_init(), TAG, "initialize default UART failed");
    }

#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    return solar_os_bus_uart_write_once(name, data, len, written, "uart");
#else
    solar_os_port_handle_t handle = SOLAR_OS_PORT_HANDLE_INIT;
    ESP_RETURN_ON_ERROR(solar_os_port_claim(name, "uart", &handle), TAG, "claim UART failed");
    esp_err_t ret = solar_os_port_write(&handle, data, len, written);
    const esp_err_t release_ret = solar_os_port_release(&handle);
    return ret == ESP_OK ? release_ret : ret;
#endif
}

esp_err_t solar_os_uart_write(const uint8_t *data, size_t len, size_t *written)
{
    return solar_os_uart_bus_write(SOLAR_OS_UART_PORT_NAME, data, len, written);
}

esp_err_t solar_os_uart_bus_read(const char *name,
                                 uint8_t *data,
                                 size_t len,
                                 uint32_t timeout_ms,
                                 size_t *read_len)
{
    if (read_len != NULL) {
        *read_len = 0;
    }
    if (name == NULL || (data == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }
    if (strcmp(name, SOLAR_OS_UART_PORT_NAME) == 0) {
        ESP_RETURN_ON_ERROR(solar_os_uart_init(), TAG, "initialize default UART failed");
    }

#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    return solar_os_bus_uart_read_once(name,
                                       data,
                                       len,
                                       timeout_ms,
                                       read_len,
                                       "uart");
#else
    solar_os_port_handle_t handle = SOLAR_OS_PORT_HANDLE_INIT;
    ESP_RETURN_ON_ERROR(solar_os_port_claim(name, "uart", &handle), TAG, "claim UART failed");
    esp_err_t ret = solar_os_port_read(&handle, data, len, timeout_ms, read_len);
    const esp_err_t release_ret = solar_os_port_release(&handle);
    return ret == ESP_OK ? release_ret : ret;
#endif
}

esp_err_t solar_os_uart_read(uint8_t *data,
                             size_t len,
                             uint32_t timeout_ms,
                             size_t *read_len)
{
    return solar_os_uart_bus_read(SOLAR_OS_UART_PORT_NAME,
                                  data,
                                  len,
                                  timeout_ms,
                                  read_len);
}

bool solar_os_uart_get_bus_status(const char *name, solar_os_uart_status_t *status)
{
    if (name == NULL || status == NULL) {
        return false;
    }
    solar_os_uart_instance_t *instance = uart_find_name(name);
    if (instance == NULL) {
        return false;
    }

    memset(status, 0, sizeof(*status));
    strlcpy(status->name, instance->name, sizeof(status->name));
    status->attached = instance->attached;
    status->initialized = instance->initialized;
    status->port_num = instance->config.port;
    status->tx_pin = instance->config.tx_pin;
    status->rx_pin = instance->config.rx_pin;
    status->baud_rate = instance->config.baud_rate;
    status->mode = instance->mode;

    if (instance->initialized && xSemaphoreTake(instance->mutex, 0) == pdTRUE) {
        size_t buffered = 0;
        if (uart_port_get_rx_buffered((uart_port_t)instance->config.port,
                                      &buffered) == ESP_OK) {
            status->rx_buffered = buffered;
            status->rx_buffered_valid = true;
        }
        xSemaphoreGive(instance->mutex);
    }

    solar_os_port_info_t port_info;
    if (solar_os_port_get_info(name, &port_info) == ESP_OK) {
        status->port_claimed = port_info.claimed;
        if (port_info.claimed) {
            strlcpy(status->port_owner, port_info.owner, sizeof(status->port_owner));
        }
    }
    return true;
}

void solar_os_uart_get_status(solar_os_uart_status_t *status)
{
    if (status == NULL) {
        return;
    }
    if (!solar_os_uart_get_bus_status(SOLAR_OS_UART_PORT_NAME, status)) {
        memset(status, 0, sizeof(*status));
        strlcpy(status->name, SOLAR_OS_UART_PORT_NAME, sizeof(status->name));
#if SOLAR_OS_BOARD_HAS_UART
        status->port_num = SOLAR_OS_BOARD_UART_PORT;
        status->tx_pin = SOLAR_OS_BOARD_PIN_UART_TX;
        status->rx_pin = SOLAR_OS_BOARD_PIN_UART_RX;
#else
        status->port_num = -1;
        status->tx_pin = -1;
        status->rx_pin = -1;
#endif
        status->baud_rate = SOLAR_OS_UART_DEFAULT_BAUD_RATE;
        status->mode = SOLAR_OS_UART_MODE_RAW;
    }
}
