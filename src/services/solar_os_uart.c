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
    char name[SOLAR_OS_BUS_NAME_MAX];
    solar_os_bus_uart_config_t config;
    solar_os_uart_mode_t mode;
} solar_os_uart_instance_t;

typedef struct {
    solar_os_uart_instance_t *instance;
    size_t index;
    uint32_t generation;
    SemaphoreHandle_t mutex;
} solar_os_uart_ref_t;

static const char *TAG = "solar_os_uart";
static solar_os_uart_instance_t uart_instances[UART_NUM_MAX];
static SemaphoreHandle_t uart_manager_mutex;
static SemaphoreHandle_t uart_instance_mutexes[UART_NUM_MAX];
static StaticSemaphore_t uart_instance_mutex_buffers[UART_NUM_MAX];
static uint32_t uart_instance_generations[UART_NUM_MAX];
static size_t uart_instance_refs[UART_NUM_MAX];
static bool uart_instance_retiring[UART_NUM_MAX];

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
    if (uart_manager_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < UART_NUM_MAX; i++) {
        if (uart_instance_mutexes[i] == NULL) {
            uart_instance_mutexes[i] =
                xSemaphoreCreateMutexStatic(&uart_instance_mutex_buffers[i]);
        }
        if (uart_instance_mutexes[i] == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static uint32_t uart_next_generation_locked(size_t index)
{
    uart_instance_generations[index]++;
    if (uart_instance_generations[index] == 0) {
        uart_instance_generations[index]++;
    }
    return uart_instance_generations[index];
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

static esp_err_t uart_pin_index_locked(size_t index, solar_os_uart_ref_t *ref)
{
    if (ref == NULL || index >= UART_NUM_MAX ||
        !uart_instances[index].active || uart_instance_retiring[index]) {
        return ESP_ERR_NOT_FOUND;
    }
    uart_instance_refs[index]++;
    *ref = (solar_os_uart_ref_t) {
        .instance = &uart_instances[index],
        .index = index,
        .generation = uart_instance_generations[index],
        .mutex = uart_instance_mutexes[index],
    };
    return ESP_OK;
}

static esp_err_t uart_pin_name_timeout(const char *name,
                                       TickType_t wait_ticks,
                                       solar_os_uart_ref_t *ref)
{
    if (name == NULL || ref == NULL || uart_manager_init() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(ref, 0, sizeof(*ref));
    xSemaphoreTake(uart_manager_mutex, portMAX_DELAY);
    solar_os_uart_instance_t *instance = uart_find_name_locked(name);
    const esp_err_t ret = instance != NULL
        ? uart_pin_index_locked((size_t)(instance - uart_instances), ref)
        : ESP_ERR_NOT_FOUND;
    xSemaphoreGive(uart_manager_mutex);
    if (ret == ESP_OK && xSemaphoreTake(ref->mutex, wait_ticks) != pdTRUE) {
        xSemaphoreTake(uart_manager_mutex, portMAX_DELAY);
        if (uart_instance_generations[ref->index] == ref->generation &&
            uart_instance_refs[ref->index] > 0) {
            uart_instance_refs[ref->index]--;
        }
        xSemaphoreGive(uart_manager_mutex);
        memset(ref, 0, sizeof(*ref));
        return ESP_ERR_TIMEOUT;
    }
    return ret;
}

static esp_err_t uart_pin_name(const char *name, solar_os_uart_ref_t *ref)
{
    return uart_pin_name_timeout(name, portMAX_DELAY, ref);
}

static esp_err_t uart_pin_instance(solar_os_uart_instance_t *instance,
                                   solar_os_uart_ref_t *ref)
{
    if (instance == NULL || ref == NULL || uart_manager_init() != ESP_OK ||
        instance < uart_instances || instance >= &uart_instances[UART_NUM_MAX]) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(ref, 0, sizeof(*ref));
    const size_t index = (size_t)(instance - uart_instances);
    xSemaphoreTake(uart_manager_mutex, portMAX_DELAY);
    const esp_err_t ret = uart_pin_index_locked(index, ref);
    xSemaphoreGive(uart_manager_mutex);
    if (ret == ESP_OK) {
        xSemaphoreTake(ref->mutex, portMAX_DELAY);
    }
    return ret;
}

static void uart_unpin(solar_os_uart_ref_t *ref)
{
    if (ref == NULL || ref->mutex == NULL) {
        return;
    }
    xSemaphoreGive(ref->mutex);
    xSemaphoreTake(uart_manager_mutex, portMAX_DELAY);
    if (ref->index < UART_NUM_MAX &&
        uart_instance_generations[ref->index] == ref->generation &&
        uart_instance_refs[ref->index] > 0) {
        uart_instance_refs[ref->index]--;
    }
    xSemaphoreGive(uart_manager_mutex);
    memset(ref, 0, sizeof(*ref));
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
    size_t instance_index = 0;
    for (size_t i = 0; i < UART_NUM_MAX; i++) {
        if (!uart_instances[i].active && !uart_instance_retiring[i] &&
            uart_instance_refs[i] == 0) {
            instance = &uart_instances[i];
            instance_index = i;
            break;
        }
    }
    if (instance == NULL) {
        xSemaphoreGive(uart_manager_mutex);
        return ESP_ERR_NO_MEM;
    }

    memset(instance, 0, sizeof(*instance));
    (void)uart_next_generation_locked(instance_index);
    instance->active = true;
    instance->persistent = persistent;
    instance->config = *config;
    instance->mode = SOLAR_OS_UART_MODE_RAW;
    strlcpy(instance->name, name, sizeof(instance->name));

    esp_err_t ret = uart_register_port(instance);
    if (ret != ESP_OK) {
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

#if SOLAR_OS_BOARD_HAS_UART
static esp_err_t uart_start_instance_locked(solar_os_uart_instance_t *instance)
{
    if (!instance->attached) {
        return ESP_ERR_INVALID_STATE;
    }
    if (instance->initialized) {
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
    return ret;
}
#endif

esp_err_t solar_os_uart_start_bus(const char *name)
{
#if !SOLAR_OS_BOARD_HAS_UART
    (void)name;
    return ESP_ERR_NOT_SUPPORTED;
#else
    solar_os_uart_ref_t ref;
    const esp_err_t pin_ret = uart_pin_name(name, &ref);
    if (pin_ret != ESP_OK) {
        return pin_ret;
    }
    const esp_err_t ret = uart_start_instance_locked(ref.instance);
    uart_unpin(&ref);
    return ret;
#endif
}

esp_err_t solar_os_uart_stop_bus(const char *name)
{
#if !SOLAR_OS_BOARD_HAS_UART
    (void)name;
    return ESP_ERR_NOT_SUPPORTED;
#else
    solar_os_uart_ref_t ref;
    esp_err_t ret = uart_pin_name(name, &ref);
    if (ret != ESP_OK) {
        return ret;
    }
    solar_os_port_info_t info;
    if (solar_os_port_get_info(name, &info) == ESP_OK && info.claimed) {
        uart_unpin(&ref);
        return ESP_ERR_INVALID_STATE;
    }
    ret = uart_stop_instance_locked(ref.instance);
    uart_unpin(&ref);
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
    const size_t index = (size_t)(instance - uart_instances);
    if (uart_instance_refs[index] > 0 || uart_instance_retiring[index]) {
        xSemaphoreGive(uart_manager_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t generation = uart_instance_generations[index];
    uart_instance_retiring[index] = true;
    xSemaphoreGive(uart_manager_mutex);

    xSemaphoreTake(uart_instance_mutexes[index], portMAX_DELAY);
    esp_err_t ret = ESP_OK;
    solar_os_port_info_t port_info;
    if (solar_os_port_get_info(name, &port_info) == ESP_OK && port_info.claimed) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        ret = uart_stop_instance_locked(instance);
        if (ret == ESP_OK && instance->attached) {
            ret = solar_os_port_unregister(name);
        }
    }
    xSemaphoreGive(uart_instance_mutexes[index]);

    xSemaphoreTake(uart_manager_mutex, portMAX_DELAY);
    if (uart_instance_generations[index] != generation ||
        !uart_instance_retiring[index]) {
        xSemaphoreGive(uart_manager_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (ret == ESP_OK) {
        memset(instance, 0, sizeof(*instance));
    }
    uart_instance_retiring[index] = false;
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
    solar_os_uart_ref_t ref;
    const esp_err_t pin_ret = uart_pin_name(name, &ref);
    if (pin_ret != ESP_OK) {
        return pin_ret;
    }
    solar_os_uart_instance_t *instance = ref.instance;
    if (instance->attached) {
        uart_unpin(&ref);
        return ESP_OK;
    }

    esp_err_t ret = uart_register_port(instance);
    if (ret == ESP_OK) {
        instance->attached = true;
        SOLAR_OS_LOGI(TAG, "%s attached", instance->name);
    }
    uart_unpin(&ref);
    return ret;
#endif
}

esp_err_t solar_os_uart_bus_detach(const char *name)
{
#if !SOLAR_OS_BOARD_HAS_UART
    (void)name;
    return ESP_ERR_NOT_SUPPORTED;
#else
    solar_os_uart_ref_t ref;
    const esp_err_t pin_ret = uart_pin_name(name, &ref);
    if (pin_ret != ESP_OK) {
        return pin_ret;
    }
    solar_os_uart_instance_t *instance = ref.instance;
    if (!instance->attached) {
        uart_unpin(&ref);
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
        SOLAR_OS_LOGI(TAG, "%s detached", instance->name);
    }
    uart_unpin(&ref);
    return ret;
#endif
}

esp_err_t solar_os_uart_init(void)
{
#if !SOLAR_OS_BOARD_HAS_UART
    return ESP_ERR_NOT_SUPPORTED;
#else
    solar_os_uart_ref_t existing;
    if (uart_pin_name(SOLAR_OS_UART_PORT_NAME, &existing) == ESP_OK) {
        uart_unpin(&existing);
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
        solar_os_uart_ref_t ref;
        if (uart_pin_name(SOLAR_OS_UART_PORT_NAME, &ref) == ESP_OK) {
            ref.instance->mode = mode;
            uart_unpin(&ref);
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
    solar_os_uart_ref_t ref;
    esp_err_t ret = uart_pin_name(name, &ref);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = uart_require_idle(name);
    if (ret != ESP_OK) {
        uart_unpin(&ref);
        return ret;
    }

    solar_os_uart_instance_t *instance = ref.instance;
    if (instance->initialized) {
        ret = uart_port_set_baud_rate((uart_port_t)instance->config.port, baud_rate);
    }
    if (ret == ESP_OK) {
        instance->config.baud_rate = baud_rate;
        if (instance->persistent) {
            ret = uart_save_config(instance);
        }
    }
    uart_unpin(&ref);
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
    solar_os_uart_ref_t ref;
    esp_err_t ret = uart_pin_name(name, &ref);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = uart_require_idle(name);
    if (ret != ESP_OK) {
        uart_unpin(&ref);
        return ret;
    }

    solar_os_uart_instance_t *instance = ref.instance;
    instance->mode = mode;
    ret = instance->persistent ? uart_save_config(instance) : ESP_OK;
    uart_unpin(&ref);
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
    solar_os_uart_ref_t ref;
    esp_err_t ret = uart_pin_instance(instance, &ref);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = uart_start_instance_locked(instance);
    if (ret == ESP_OK) {
        ret = uart_port_write((uart_port_t)instance->config.port,
                              data,
                              len,
                              written);
    }
    uart_unpin(&ref);
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
    solar_os_uart_ref_t ref;
    esp_err_t ret = uart_pin_instance(instance, &ref);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = uart_start_instance_locked(instance);
    if (ret == ESP_OK) {
        ret = instance->mode == SOLAR_OS_UART_MODE_LINE
            ? uart_read_line_mode(instance, data, len, timeout_ms, read_len)
            : uart_port_read((uart_port_t)instance->config.port,
                             data,
                             len,
                             timeout_ms,
                             read_len);
    }
    uart_unpin(&ref);
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
    if (instance == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_uart_ref_t ref;
    esp_err_t ret = uart_pin_instance(instance, &ref);
    if (ret == ESP_OK) {
        ret = uart_start_instance_locked(instance);
        uart_unpin(&ref);
    }
    return ret;
}

static esp_err_t uart_port_close_cb(void *user)
{
    solar_os_uart_instance_t *instance = (solar_os_uart_instance_t *)user;
    if (instance == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_uart_ref_t ref;
    esp_err_t ret = uart_pin_instance(instance, &ref);
    if (ret == ESP_OK) {
        ret = uart_stop_instance_locked(instance);
        uart_unpin(&ref);
    }
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
    solar_os_uart_ref_t ref;
    if (uart_pin_name_timeout(name, 0, &ref) != ESP_OK) {
        return false;
    }
    solar_os_uart_instance_t *instance = ref.instance;

    memset(status, 0, sizeof(*status));
    strlcpy(status->name, instance->name, sizeof(status->name));
    status->attached = instance->attached;
    status->initialized = instance->initialized;
    status->port_num = instance->config.port;
    status->tx_pin = instance->config.tx_pin;
    status->rx_pin = instance->config.rx_pin;
    status->baud_rate = instance->config.baud_rate;
    status->mode = instance->mode;

    if (instance->initialized) {
        size_t buffered = 0;
        if (uart_port_get_rx_buffered((uart_port_t)instance->config.port,
                                      &buffered) == ESP_OK) {
            status->rx_buffered = buffered;
            status->rx_buffered_valid = true;
        }
    }
    uart_unpin(&ref);

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
