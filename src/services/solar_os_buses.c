#include "solar_os_buses.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "routed_spi_bus.h"
#include "solar_os_board.h"
#include "solar_os_board_caps.h"
#include "solar_os_config.h"
#include "solar_os_pins.h"
#include "solar_os_resources.h"
#if SOLAR_OS_PACKAGE_SERVICE_I2C && SOLAR_OS_BOARD_HAS_I2C
#include "i2c_bus.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_ONEWIRE
#include "solar_os_onewire.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_UART && SOLAR_OS_BOARD_HAS_UART
#include "solar_os_port.h"
#include "solar_os_uart.h"
#endif

#define SOLAR_OS_BUS_MAX 8
#define SOLAR_OS_BUS_LEASE_MAX 16

typedef struct {
    bool active;
    size_t bus_index;
    char owner[SOLAR_OS_BUS_OWNER_MAX];
    size_t ref_count;
#if SOLAR_OS_PACKAGE_SERVICE_UART && SOLAR_OS_BOARD_HAS_UART
    solar_os_port_handle_t port;
#endif
} solar_os_bus_lease_t;

static solar_os_bus_info_t buses[SOLAR_OS_BUS_MAX];
static solar_os_bus_lease_t leases[SOLAR_OS_BUS_LEASE_MAX];
static bool buses_initialized_here[SOLAR_OS_BUS_MAX];
#if SOLAR_OS_PACKAGE_SERVICE_I2C && SOLAR_OS_BOARD_HAS_I2C
static i2c_master_bus_handle_t buses_i2c_handles[SOLAR_OS_BUS_MAX];
#endif
static SemaphoreHandle_t buses_mutex;
static StaticSemaphore_t buses_mutex_buffer;
static bool buses_initialized;

static const solar_os_bus_definition_t board_buses[] = SOLAR_OS_BOARD_BUSES;

static bool protocol_valid(solar_os_bus_protocol_t protocol)
{
    return protocol >= SOLAR_OS_BUS_PROTOCOL_I2C &&
        protocol <= SOLAR_OS_BUS_PROTOCOL_ONEWIRE;
}

static bool name_valid(const char *name)
{
    return name != NULL &&
        name[0] != '\0' &&
        strnlen(name, SOLAR_OS_BUS_NAME_MAX) < SOLAR_OS_BUS_NAME_MAX;
}

static bool owner_valid(const char *owner)
{
    return owner != NULL &&
        owner[0] != '\0' &&
        strnlen(owner, SOLAR_OS_BUS_OWNER_MAX) < SOLAR_OS_BUS_OWNER_MAX;
}

static esp_err_t ensure_mutex(void)
{
    if (buses_mutex == NULL) {
        buses_mutex = xSemaphoreCreateMutexStatic(&buses_mutex_buffer);
    }
    return buses_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static int find_bus_index_locked(const char *name)
{
    for (size_t i = 0; i < SOLAR_OS_BUS_MAX; i++) {
        if (buses[i].active && strcmp(buses[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static size_t lease_count_locked(size_t bus_index)
{
    size_t count = 0;
    for (size_t i = 0; i < SOLAR_OS_BUS_LEASE_MAX; i++) {
        if (leases[i].active && leases[i].bus_index == bus_index) {
            count += leases[i].ref_count;
        }
    }
    return count;
}

static bool runtime_spi_host_allowed(int host)
{
    return host >= 0 && host < 32 &&
        (SOLAR_OS_BOARD_RUNTIME_SPI_HOST_MASK & (1U << (unsigned)host)) != 0;
}

static bool runtime_uart_port_allowed(int port)
{
    return port >= 0 && port < 32 &&
        (SOLAR_OS_BOARD_RUNTIME_UART_PORT_MASK & (1U << (unsigned)port)) != 0;
}

static bool spi_host_registered_locked(int host)
{
    for (size_t i = 0; i < SOLAR_OS_BUS_MAX; i++) {
        if (buses[i].active &&
            buses[i].protocol == SOLAR_OS_BUS_PROTOCOL_SPI &&
            buses[i].config.spi.host == host) {
            return true;
        }
    }
    return false;
}

static bool i2c_port_registered_locked(int port)
{
    for (size_t i = 0; i < SOLAR_OS_BUS_MAX; i++) {
        if (buses[i].active &&
            buses[i].protocol == SOLAR_OS_BUS_PROTOCOL_I2C &&
            buses[i].config.i2c.port == port) {
            return true;
        }
    }
    return false;
}

static bool uart_port_registered_locked(int port)
{
    for (size_t i = 0; i < SOLAR_OS_BUS_MAX; i++) {
        if (buses[i].active &&
            buses[i].protocol == SOLAR_OS_BUS_PROTOCOL_UART &&
            buses[i].config.uart.port == port) {
            return true;
        }
    }
    return false;
}

static bool spi_cs_allowed(const solar_os_bus_spi_config_t *config, int pin)
{
    for (size_t i = 0; config != NULL && i < config->cs_count; i++) {
        if (config->cs[i].pin == pin) {
            return true;
        }
    }
    return false;
}

static void runtime_bus_owner(const char *name, char *owner, size_t owner_size)
{
    if (owner == NULL || owner_size == 0) {
        return;
    }
    owner[0] = '\0';
    if (name != NULL) {
        strlcpy(owner, "bus:", owner_size);
        strlcat(owner, name, owner_size);
    }
}

static bool definition_valid(const solar_os_bus_definition_t *definition)
{
    if (definition == NULL ||
        !name_valid(definition->name) ||
        !protocol_valid(definition->protocol) ||
        definition->origin > SOLAR_OS_BUS_ORIGIN_RUNTIME ||
        definition->sharing > SOLAR_OS_BUS_EXCLUSIVE) {
        return false;
    }

    bool config_valid = false;
    switch (definition->protocol) {
    case SOLAR_OS_BUS_PROTOCOL_I2C:
        config_valid = definition->config.i2c.port >= 0 &&
#if SOLAR_OS_PACKAGE_SERVICE_I2C && SOLAR_OS_BOARD_HAS_I2C
            definition->config.i2c.port < I2C_NUM_MAX &&
#endif
            definition->config.i2c.sda_pin >= 0 &&
            definition->config.i2c.scl_pin >= 0 &&
            definition->config.i2c.sda_pin != definition->config.i2c.scl_pin &&
            definition->config.i2c.speed_hz > 0;
        break;
    case SOLAR_OS_BUS_PROTOCOL_SPI:
        config_valid = definition->config.spi.host >= 0 &&
            definition->config.spi.sclk_pin >= 0 &&
            definition->config.spi.mosi_pin >= 0 &&
            definition->config.spi.miso_pin >= -1 &&
            definition->config.spi.sclk_pin != definition->config.spi.mosi_pin &&
            (definition->config.spi.miso_pin < 0 ||
             (definition->config.spi.miso_pin != definition->config.spi.sclk_pin &&
              definition->config.spi.miso_pin != definition->config.spi.mosi_pin)) &&
            definition->config.spi.max_transfer_size > 0 &&
            definition->config.spi.cs_count <= SOLAR_OS_BUS_SPI_CS_MAX;
        for (size_t i = 0; config_valid && i < definition->config.spi.cs_count; i++) {
            config_valid = name_valid(definition->config.spi.cs[i].name) &&
                definition->config.spi.cs[i].pin >= 0 &&
                definition->config.spi.cs[i].pin != definition->config.spi.sclk_pin &&
                definition->config.spi.cs[i].pin != definition->config.spi.mosi_pin &&
                definition->config.spi.cs[i].pin != definition->config.spi.miso_pin;
            for (size_t j = 0; config_valid && j < i; j++) {
                config_valid = definition->config.spi.cs[j].pin != definition->config.spi.cs[i].pin &&
                    strcmp(definition->config.spi.cs[j].name,
                           definition->config.spi.cs[i].name) != 0;
            }
        }
        break;
    case SOLAR_OS_BUS_PROTOCOL_UART:
        config_valid = definition->config.uart.port >= 0 &&
#if SOLAR_OS_PACKAGE_SERVICE_UART && SOLAR_OS_BOARD_HAS_UART
            definition->config.uart.port < UART_NUM_MAX &&
#endif
            definition->config.uart.tx_pin >= 0 &&
            definition->config.uart.rx_pin >= 0 &&
            definition->config.uart.tx_pin != definition->config.uart.rx_pin &&
            definition->config.uart.baud_rate >= SOLAR_OS_BUS_UART_MIN_BAUD_RATE &&
            definition->config.uart.baud_rate <= SOLAR_OS_BUS_UART_MAX_BAUD_RATE;
        break;
    case SOLAR_OS_BUS_PROTOCOL_ONEWIRE:
        config_valid = definition->config.onewire.pin >= 0;
        break;
    default:
        return false;
    }
    if (!config_valid || definition->origin == SOLAR_OS_BUS_ORIGIN_BOARD) {
        return config_valid;
    }

    switch (definition->protocol) {
    case SOLAR_OS_BUS_PROTOCOL_I2C:
        return solar_os_pin_is_routable(definition->config.i2c.sda_pin) &&
            solar_os_pin_is_routable(definition->config.i2c.scl_pin);
    case SOLAR_OS_BUS_PROTOCOL_SPI:
        if (definition->config.spi.cs_count == 0 ||
            !runtime_spi_host_allowed(definition->config.spi.host) ||
            !solar_os_pin_is_routable(definition->config.spi.sclk_pin) ||
            !solar_os_pin_is_routable(definition->config.spi.mosi_pin) ||
            (definition->config.spi.miso_pin >= 0 &&
             !solar_os_pin_is_routable(definition->config.spi.miso_pin))) {
            return false;
        }
        for (size_t i = 0; i < definition->config.spi.cs_count; i++) {
            if (!solar_os_pin_is_routable(definition->config.spi.cs[i].pin)) {
                return false;
            }
        }
        return true;
    case SOLAR_OS_BUS_PROTOCOL_UART:
        return runtime_uart_port_allowed(definition->config.uart.port) &&
            solar_os_pin_is_routable(definition->config.uart.tx_pin) &&
            solar_os_pin_is_routable(definition->config.uart.rx_pin);
    case SOLAR_OS_BUS_PROTOCOL_ONEWIRE:
        return solar_os_pin_is_routable(definition->config.onewire.pin);
    default:
        return false;
    }
}

static esp_err_t register_locked(const solar_os_bus_definition_t *definition)
{
    if (!definition_valid(definition)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (find_bus_index_locked(definition->name) >= 0) {
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t i = 0; i < SOLAR_OS_BUS_MAX; i++) {
        if (buses[i].active) {
            continue;
        }
        buses[i] = (solar_os_bus_info_t) {
            .active = true,
            .id = i,
            .protocol = definition->protocol,
            .origin = definition->origin,
            .sharing = definition->sharing,
            .config = definition->config,
        };
        strlcpy(buses[i].name, definition->name, sizeof(buses[i].name));
        return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
}

static esp_err_t register_board_bus_locked(const solar_os_bus_definition_t *definition)
{
    if (definition == NULL || definition->name == NULL || definition->name[0] == '\0') {
        return ESP_OK;
    }
    return definition->origin == SOLAR_OS_BUS_ORIGIN_BOARD
        ? register_locked(definition)
        : ESP_ERR_INVALID_ARG;
}

static esp_err_t start_i2c_locked(size_t bus_index)
{
#if SOLAR_OS_PACKAGE_SERVICE_I2C && SOLAR_OS_BOARD_HAS_I2C
    const solar_os_bus_i2c_config_t *config = &buses[bus_index].config.i2c;
    const i2c_bus_config_t driver_config = {
        .port = config->port,
        .sda_pin = (gpio_num_t)config->sda_pin,
        .scl_pin = (gpio_num_t)config->scl_pin,
        .speed_hz = config->speed_hz,
    };
    esp_err_t ret;
    if (buses[bus_index].origin == SOLAR_OS_BUS_ORIGIN_BOARD) {
        ret = i2c_bus_init_config(&driver_config);
        if (ret == ESP_OK) {
            buses_i2c_handles[bus_index] = i2c_bus_get_handle();
            buses_initialized_here[bus_index] = false;
        }
    } else {
        ret = i2c_bus_start_config(&driver_config,
                                   false,
                                   &buses_i2c_handles[bus_index],
                                   &buses_initialized_here[bus_index]);
    }
    if (ret == ESP_OK) {
        buses[bus_index].ready = true;
    }
    return ret;
#else
    (void)bus_index;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t stop_i2c_locked(size_t bus_index)
{
#if SOLAR_OS_PACKAGE_SERVICE_I2C && SOLAR_OS_BOARD_HAS_I2C
    if (buses_i2c_handles[bus_index] == NULL) {
        buses[bus_index].ready = false;
        buses_initialized_here[bus_index] = false;
        return ESP_OK;
    }
    const solar_os_bus_i2c_config_t *config = &buses[bus_index].config.i2c;
    const i2c_bus_config_t driver_config = {
        .port = config->port,
        .sda_pin = (gpio_num_t)config->sda_pin,
        .scl_pin = (gpio_num_t)config->scl_pin,
        .speed_hz = config->speed_hz,
    };
    const esp_err_t ret = i2c_bus_stop_config(&driver_config,
                                               buses_i2c_handles[bus_index],
                                               buses_initialized_here[bus_index]);
    if (ret == ESP_OK) {
        buses_i2c_handles[bus_index] = NULL;
        buses[bus_index].ready = false;
        buses_initialized_here[bus_index] = false;
    }
    return ret;
#else
    (void)bus_index;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t start_onewire_locked(size_t bus_index)
{
#if SOLAR_OS_PACKAGE_SERVICE_ONEWIRE
    const esp_err_t ret = solar_os_onewire_init();
    if (ret == ESP_OK) {
        buses[bus_index].ready = true;
    }
    return ret;
#else
    (void)bus_index;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t solar_os_buses_init(void)
{
    esp_err_t ret = ensure_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(buses_mutex, portMAX_DELAY);
    if (buses_initialized) {
        xSemaphoreGive(buses_mutex);
        return ESP_OK;
    }

    for (size_t i = 0; ret == ESP_OK && i < sizeof(board_buses) / sizeof(board_buses[0]); i++) {
        ret = register_board_bus_locked(&board_buses[i]);
    }
    if (ret == ESP_OK) {
        buses_initialized = true;
    } else {
        memset(buses, 0, sizeof(buses));
    }
    xSemaphoreGive(buses_mutex);
    return ret;
}

esp_err_t solar_os_bus_register(const solar_os_bus_definition_t *definition)
{
    esp_err_t ret = solar_os_buses_init();
    if (ret != ESP_OK) {
        return ret;
    }
    if (definition == NULL || definition->origin != SOLAR_OS_BUS_ORIGIN_RUNTIME) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!definition_valid(definition)) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (definition->protocol) {
    case SOLAR_OS_BUS_PROTOCOL_SPI:
        break;
    case SOLAR_OS_BUS_PROTOCOL_I2C:
#if SOLAR_OS_PACKAGE_SERVICE_I2C && SOLAR_OS_BOARD_HAS_I2C
        break;
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif
    case SOLAR_OS_BUS_PROTOCOL_UART:
#if SOLAR_OS_PACKAGE_SERVICE_UART && SOLAR_OS_BOARD_HAS_UART
        break;
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif
    case SOLAR_OS_BUS_PROTOCOL_ONEWIRE:
#if SOLAR_OS_PACKAGE_SERVICE_ONEWIRE
        break;
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    char owner[SOLAR_OS_RESOURCE_OWNER_MAX];
    runtime_bus_owner(definition->name, owner, sizeof(owner));
    solar_os_resource_request_t requests[3];
    size_t request_count = 0;
    switch (definition->protocol) {
    case SOLAR_OS_BUS_PROTOCOL_SPI:
        requests[request_count++] = (solar_os_resource_request_t) {
            .kind = SOLAR_OS_RESOURCE_GPIO_PIN,
            .primary = definition->config.spi.sclk_pin,
            .secondary = -1,
            .label = "spi-sclk",
        };
        requests[request_count++] = (solar_os_resource_request_t) {
            .kind = SOLAR_OS_RESOURCE_GPIO_PIN,
            .primary = definition->config.spi.mosi_pin,
            .secondary = -1,
            .label = "spi-mosi",
        };
        if (definition->config.spi.miso_pin >= 0) {
            requests[request_count++] = (solar_os_resource_request_t) {
                .kind = SOLAR_OS_RESOURCE_GPIO_PIN,
                .primary = definition->config.spi.miso_pin,
                .secondary = -1,
                .label = "spi-miso",
            };
        }
        break;
    case SOLAR_OS_BUS_PROTOCOL_I2C:
        requests[request_count++] = (solar_os_resource_request_t) {
            .kind = SOLAR_OS_RESOURCE_GPIO_PIN,
            .primary = definition->config.i2c.sda_pin,
            .secondary = -1,
            .label = "i2c-sda",
        };
        requests[request_count++] = (solar_os_resource_request_t) {
            .kind = SOLAR_OS_RESOURCE_GPIO_PIN,
            .primary = definition->config.i2c.scl_pin,
            .secondary = -1,
            .label = "i2c-scl",
        };
        break;
    case SOLAR_OS_BUS_PROTOCOL_UART:
        requests[request_count++] = (solar_os_resource_request_t) {
            .kind = SOLAR_OS_RESOURCE_UART_PORT,
            .primary = definition->config.uart.port,
            .secondary = -1,
            .label = "uart-port",
        };
        requests[request_count++] = (solar_os_resource_request_t) {
            .kind = SOLAR_OS_RESOURCE_GPIO_PIN,
            .primary = definition->config.uart.tx_pin,
            .secondary = -1,
            .label = "uart-tx",
        };
        requests[request_count++] = (solar_os_resource_request_t) {
            .kind = SOLAR_OS_RESOURCE_GPIO_PIN,
            .primary = definition->config.uart.rx_pin,
            .secondary = -1,
            .label = "uart-rx",
        };
        break;
    case SOLAR_OS_BUS_PROTOCOL_ONEWIRE:
        requests[request_count++] = (solar_os_resource_request_t) {
            .kind = SOLAR_OS_RESOURCE_GPIO_PIN,
            .primary = definition->config.onewire.pin,
            .secondary = -1,
            .label = "onewire",
        };
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    bool pins_claimed = false;
    xSemaphoreTake(buses_mutex, portMAX_DELAY);
    const bool endpoint_registered = definition->protocol == SOLAR_OS_BUS_PROTOCOL_SPI
        ? spi_host_registered_locked(definition->config.spi.host)
        : definition->protocol == SOLAR_OS_BUS_PROTOCOL_I2C
            ? i2c_port_registered_locked(definition->config.i2c.port)
            : definition->protocol == SOLAR_OS_BUS_PROTOCOL_UART
                ? uart_port_registered_locked(definition->config.uart.port)
                : false;
    if (find_bus_index_locked(definition->name) >= 0 || endpoint_registered) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        ret = solar_os_resource_claim_bundle(requests, request_count, owner, NULL);
        if (ret == ESP_OK) {
            pins_claimed = true;
            ret = register_locked(definition);
#if SOLAR_OS_PACKAGE_SERVICE_UART && SOLAR_OS_BOARD_HAS_UART
            if (ret == ESP_OK && definition->protocol == SOLAR_OS_BUS_PROTOCOL_UART) {
                ret = solar_os_uart_register_bus(definition->name,
                                                 &definition->config.uart,
                                                 false);
                if (ret != ESP_OK) {
                    const int index = find_bus_index_locked(definition->name);
                    if (index >= 0) {
                        memset(&buses[index], 0, sizeof(buses[index]));
                    }
                }
            }
#endif
        }
    }
    if (ret != ESP_OK && pins_claimed) {
        (void)solar_os_resource_release_owner(owner);
    }
    xSemaphoreGive(buses_mutex);
    return ret;
}

esp_err_t solar_os_bus_unregister(const char *name)
{
    if (!name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_buses_init();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(buses_mutex, portMAX_DELAY);
    const int index = find_bus_index_locked(name);
    if (index < 0) {
        ret = ESP_ERR_NOT_FOUND;
    } else if (buses[index].origin != SOLAR_OS_BUS_ORIGIN_RUNTIME) {
        ret = ESP_ERR_NOT_ALLOWED;
    } else if (lease_count_locked((size_t)index) > 0) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
#if SOLAR_OS_PACKAGE_SERVICE_UART && SOLAR_OS_BOARD_HAS_UART
        if (buses[index].protocol == SOLAR_OS_BUS_PROTOCOL_UART) {
            ret = solar_os_uart_unregister_bus(buses[index].name);
            if (ret != ESP_OK) {
                xSemaphoreGive(buses_mutex);
                return ret;
            }
        }
#endif
        if (buses[index].protocol == SOLAR_OS_BUS_PROTOCOL_I2C && buses[index].ready) {
            ret = stop_i2c_locked((size_t)index);
            if (ret != ESP_OK) {
                xSemaphoreGive(buses_mutex);
                return ret;
            }
        }
        char owner[SOLAR_OS_RESOURCE_OWNER_MAX];
        runtime_bus_owner(buses[index].name, owner, sizeof(owner));
        memset(&buses[index], 0, sizeof(buses[index]));
#if SOLAR_OS_PACKAGE_SERVICE_I2C && SOLAR_OS_BOARD_HAS_I2C
        buses_i2c_handles[index] = NULL;
#endif
        buses_initialized_here[index] = false;
        (void)solar_os_resource_release_owner(owner);
        ret = ESP_OK;
    }
    xSemaphoreGive(buses_mutex);
    return ret;
}

size_t solar_os_bus_count(void)
{
    if (solar_os_buses_init() != ESP_OK) {
        return 0;
    }
    size_t count = 0;
    xSemaphoreTake(buses_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SOLAR_OS_BUS_MAX; i++) {
        if (buses[i].active) {
            count++;
        }
    }
    xSemaphoreGive(buses_mutex);
    return count;
}

size_t solar_os_bus_count_protocol(solar_os_bus_protocol_t protocol)
{
    if (!protocol_valid(protocol) || solar_os_buses_init() != ESP_OK) {
        return 0;
    }
    size_t count = 0;
    xSemaphoreTake(buses_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SOLAR_OS_BUS_MAX; i++) {
        if (buses[i].active && buses[i].protocol == protocol) {
            count++;
        }
    }
    xSemaphoreGive(buses_mutex);
    return count;
}

static void refresh_uart_config(solar_os_bus_info_t *info)
{
#if SOLAR_OS_PACKAGE_SERVICE_UART && SOLAR_OS_BOARD_HAS_UART
    if (info != NULL && info->protocol == SOLAR_OS_BUS_PROTOCOL_UART) {
        solar_os_uart_status_t status;
        if (solar_os_uart_get_bus_status(info->name, &status)) {
            info->config.uart.baud_rate = status.baud_rate;
            info->ready = status.initialized;
        }
    }
#else
    (void)info;
#endif
}

bool solar_os_bus_get(size_t index, solar_os_bus_info_t *info)
{
    if (info == NULL || solar_os_buses_init() != ESP_OK) {
        return false;
    }
    size_t current = 0;
    bool found = false;
    xSemaphoreTake(buses_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SOLAR_OS_BUS_MAX; i++) {
        if (!buses[i].active) {
            continue;
        }
        if (current++ == index) {
            *info = buses[i];
            info->lease_count = lease_count_locked(i);
            found = true;
            break;
        }
    }
    xSemaphoreGive(buses_mutex);
    if (found) {
        refresh_uart_config(info);
    }
    return found;
}

bool solar_os_bus_get_protocol(solar_os_bus_protocol_t protocol,
                               size_t index,
                               solar_os_bus_info_t *info)
{
    if (info == NULL || !protocol_valid(protocol) || solar_os_buses_init() != ESP_OK) {
        return false;
    }
    size_t current = 0;
    bool found = false;
    xSemaphoreTake(buses_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SOLAR_OS_BUS_MAX; i++) {
        if (!buses[i].active || buses[i].protocol != protocol) {
            continue;
        }
        if (current++ == index) {
            *info = buses[i];
            info->lease_count = lease_count_locked(i);
            found = true;
            break;
        }
    }
    xSemaphoreGive(buses_mutex);
    if (found) {
        refresh_uart_config(info);
    }
    return found;
}

bool solar_os_bus_find(const char *name,
                       solar_os_bus_protocol_t protocol,
                       solar_os_bus_info_t *info)
{
    if (!name_valid(name) || !protocol_valid(protocol) || solar_os_buses_init() != ESP_OK) {
        return false;
    }
    bool found = false;
    xSemaphoreTake(buses_mutex, portMAX_DELAY);
    const int index = find_bus_index_locked(name);
    if (index >= 0 && buses[index].protocol == protocol) {
        if (info != NULL) {
            *info = buses[index];
            info->lease_count = lease_count_locked((size_t)index);
        }
        found = true;
    }
    xSemaphoreGive(buses_mutex);
    if (found && info != NULL) {
        refresh_uart_config(info);
    }
    return found;
}

esp_err_t solar_os_bus_acquire(const char *name,
                               solar_os_bus_protocol_t protocol,
                               const char *owner)
{
    if (!name_valid(name) || !protocol_valid(protocol) || !owner_valid(owner)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_buses_init();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(buses_mutex, portMAX_DELAY);
    const int bus_index = find_bus_index_locked(name);
    if (bus_index < 0 || buses[bus_index].protocol != protocol) {
        xSemaphoreGive(buses_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    solar_os_bus_lease_t *free_lease = NULL;
    for (size_t i = 0; i < SOLAR_OS_BUS_LEASE_MAX; i++) {
        solar_os_bus_lease_t *lease = &leases[i];
        if (!lease->active) {
            if (free_lease == NULL) {
                free_lease = lease;
            }
            continue;
        }
        if (lease->bus_index != (size_t)bus_index) {
            continue;
        }
        if (strcmp(lease->owner, owner) == 0) {
            lease->ref_count++;
            xSemaphoreGive(buses_mutex);
            return ESP_OK;
        }
        if (buses[bus_index].sharing == SOLAR_OS_BUS_EXCLUSIVE) {
            xSemaphoreGive(buses_mutex);
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (free_lease == NULL) {
        xSemaphoreGive(buses_mutex);
        return ESP_ERR_NO_MEM;
    }
#if SOLAR_OS_PACKAGE_SERVICE_UART && SOLAR_OS_BOARD_HAS_UART
    solar_os_port_handle_t uart_port = SOLAR_OS_PORT_HANDLE_INIT;
#endif
    if (lease_count_locked((size_t)bus_index) == 0) {
        if (buses[bus_index].protocol == SOLAR_OS_BUS_PROTOCOL_I2C) {
            if (!buses[bus_index].ready) {
                ret = start_i2c_locked((size_t)bus_index);
            }
        } else if (buses[bus_index].protocol == SOLAR_OS_BUS_PROTOCOL_SPI) {
            bool initialized_here = false;
            ret = solar_os_routed_spi_start(&buses[bus_index].config.spi,
                                            buses[bus_index].origin == SOLAR_OS_BUS_ORIGIN_BOARD,
                                            &initialized_here);
            if (ret == ESP_OK) {
                buses[bus_index].ready = true;
                buses_initialized_here[bus_index] = initialized_here;
            }
        } else if (buses[bus_index].protocol == SOLAR_OS_BUS_PROTOCOL_ONEWIRE) {
            ret = start_onewire_locked((size_t)bus_index);
#if SOLAR_OS_PACKAGE_SERVICE_UART && SOLAR_OS_BOARD_HAS_UART
        } else if (buses[bus_index].protocol == SOLAR_OS_BUS_PROTOCOL_UART) {
            ret = solar_os_port_claim(buses[bus_index].name, owner, &uart_port);
            if (ret == ESP_OK) {
                buses[bus_index].ready = true;
            }
#endif
        }
        if (ret != ESP_OK) {
            xSemaphoreGive(buses_mutex);
            return ret;
        }
    }
    *free_lease = (solar_os_bus_lease_t) {
        .active = true,
        .bus_index = (size_t)bus_index,
        .ref_count = 1,
#if SOLAR_OS_PACKAGE_SERVICE_UART && SOLAR_OS_BOARD_HAS_UART
        .port = uart_port,
#endif
    };
    strlcpy(free_lease->owner, owner, sizeof(free_lease->owner));
    xSemaphoreGive(buses_mutex);
    return ESP_OK;
}

esp_err_t solar_os_bus_release(const char *name,
                               solar_os_bus_protocol_t protocol,
                               const char *owner)
{
    if (!name_valid(name) || !protocol_valid(protocol) || !owner_valid(owner)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_buses_init();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(buses_mutex, portMAX_DELAY);
    const int bus_index = find_bus_index_locked(name);
    if (bus_index < 0 || buses[bus_index].protocol != protocol) {
        xSemaphoreGive(buses_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    for (size_t i = 0; i < SOLAR_OS_BUS_LEASE_MAX; i++) {
        solar_os_bus_lease_t *lease = &leases[i];
        if (!lease->active ||
            lease->bus_index != (size_t)bus_index ||
            strcmp(lease->owner, owner) != 0) {
            continue;
        }
        if (--lease->ref_count == 0) {
#if SOLAR_OS_PACKAGE_SERVICE_UART && SOLAR_OS_BOARD_HAS_UART
            if (buses[bus_index].protocol == SOLAR_OS_BUS_PROTOCOL_UART &&
                solar_os_port_handle_valid(&lease->port)) {
                ret = solar_os_port_release(&lease->port);
                if (ret != ESP_OK) {
                    lease->ref_count = 1;
                    xSemaphoreGive(buses_mutex);
                    return ret;
                }
                if (buses[bus_index].origin == SOLAR_OS_BUS_ORIGIN_RUNTIME) {
                    buses[bus_index].ready = false;
                }
            }
#endif
            if (lease_count_locked((size_t)bus_index) == 0 &&
                buses[bus_index].protocol == SOLAR_OS_BUS_PROTOCOL_SPI) {
                ret = solar_os_routed_spi_stop(&buses[bus_index].config.spi,
                                               buses_initialized_here[bus_index]);
                if (ret != ESP_OK) {
                    lease->ref_count = 1;
                    xSemaphoreGive(buses_mutex);
                    return ret;
                }
                buses[bus_index].ready = false;
                buses_initialized_here[bus_index] = false;
            } else if (lease_count_locked((size_t)bus_index) == 0 &&
                       buses[bus_index].protocol == SOLAR_OS_BUS_PROTOCOL_ONEWIRE) {
                buses[bus_index].ready = false;
            }
            memset(lease, 0, sizeof(*lease));
        }
        xSemaphoreGive(buses_mutex);
        return ESP_OK;
    }
    xSemaphoreGive(buses_mutex);
    return ESP_ERR_NOT_FOUND;
}

size_t solar_os_bus_release_owner(const char *owner)
{
    if (!owner_valid(owner) || solar_os_buses_init() != ESP_OK) {
        return 0;
    }
    size_t released = 0;
    xSemaphoreTake(buses_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SOLAR_OS_BUS_LEASE_MAX; i++) {
        if (!leases[i].active || strcmp(leases[i].owner, owner) != 0) {
            continue;
        }
        const size_t bus_index = leases[i].bus_index;
#if SOLAR_OS_PACKAGE_SERVICE_UART && SOLAR_OS_BOARD_HAS_UART
        if (buses[bus_index].protocol == SOLAR_OS_BUS_PROTOCOL_UART &&
            solar_os_port_handle_valid(&leases[i].port)) {
            if (solar_os_port_release(&leases[i].port) != ESP_OK) {
                continue;
            }
            if (buses[bus_index].origin == SOLAR_OS_BUS_ORIGIN_RUNTIME) {
                buses[bus_index].ready = false;
            }
        }
#endif
        if (lease_count_locked(bus_index) == leases[i].ref_count &&
            buses[bus_index].protocol == SOLAR_OS_BUS_PROTOCOL_SPI) {
            const esp_err_t ret = solar_os_routed_spi_stop(&buses[bus_index].config.spi,
                                                           buses_initialized_here[bus_index]);
            if (ret != ESP_OK) {
                continue;
            }
            buses[bus_index].ready = false;
            buses_initialized_here[bus_index] = false;
        } else if (lease_count_locked(bus_index) == leases[i].ref_count &&
                   buses[bus_index].protocol == SOLAR_OS_BUS_PROTOCOL_ONEWIRE) {
            buses[bus_index].ready = false;
        }
        released += leases[i].ref_count;
        memset(&leases[i], 0, sizeof(leases[i]));
    }
    xSemaphoreGive(buses_mutex);
    return released;
}

static esp_err_t find_ready_i2c_locked(const char *name, size_t *bus_index)
{
    const int index = find_bus_index_locked(name);
    if (index < 0 || buses[index].protocol != SOLAR_OS_BUS_PROTOCOL_I2C) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!buses[index].ready || lease_count_locked((size_t)index) == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (bus_index != NULL) {
        *bus_index = (size_t)index;
    }
    return ESP_OK;
}

esp_err_t solar_os_bus_i2c_probe(const char *name, uint8_t address)
{
    if (!name_valid(name) || address > 0x7fU) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_buses_init();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(buses_mutex, portMAX_DELAY);
    size_t bus_index = 0;
    ret = find_ready_i2c_locked(name, &bus_index);
#if SOLAR_OS_PACKAGE_SERVICE_I2C && SOLAR_OS_BOARD_HAS_I2C
    if (ret == ESP_OK) {
        ret = i2c_bus_probe_handle(buses_i2c_handles[bus_index], address);
    }
#else
    if (ret == ESP_OK) {
        ret = ESP_ERR_NOT_SUPPORTED;
    }
#endif
    xSemaphoreGive(buses_mutex);
    return ret;
}

esp_err_t solar_os_bus_i2c_read_reg(const char *name,
                                    uint8_t address,
                                    uint8_t reg,
                                    uint8_t *data,
                                    size_t len)
{
    if (!name_valid(name) || address > 0x7fU || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_buses_init();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(buses_mutex, portMAX_DELAY);
    size_t bus_index = 0;
    ret = find_ready_i2c_locked(name, &bus_index);
#if SOLAR_OS_PACKAGE_SERVICE_I2C && SOLAR_OS_BOARD_HAS_I2C
    if (ret == ESP_OK) {
        ret = i2c_bus_read_reg_handle(buses_i2c_handles[bus_index],
                                      buses[bus_index].config.i2c.speed_hz,
                                      address,
                                      reg,
                                      data,
                                      len);
    }
#else
    if (ret == ESP_OK) {
        ret = ESP_ERR_NOT_SUPPORTED;
    }
#endif
    xSemaphoreGive(buses_mutex);
    return ret;
}

esp_err_t solar_os_bus_i2c_write_reg(const char *name,
                                     uint8_t address,
                                     uint8_t reg,
                                     const uint8_t *data,
                                     size_t len)
{
    if (!name_valid(name) || address > 0x7fU || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_buses_init();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(buses_mutex, portMAX_DELAY);
    size_t bus_index = 0;
    ret = find_ready_i2c_locked(name, &bus_index);
#if SOLAR_OS_PACKAGE_SERVICE_I2C && SOLAR_OS_BOARD_HAS_I2C
    if (ret == ESP_OK) {
        ret = i2c_bus_write_reg_handle(buses_i2c_handles[bus_index],
                                       buses[bus_index].config.i2c.speed_hz,
                                       address,
                                       reg,
                                       data,
                                       len);
    }
#else
    if (ret == ESP_OK) {
        ret = ESP_ERR_NOT_SUPPORTED;
    }
#endif
    xSemaphoreGive(buses_mutex);
    return ret;
}

static esp_err_t find_ready_uart_locked(const char *name,
                                        solar_os_bus_lease_t **bus_lease)
{
    const int index = find_bus_index_locked(name);
    if (index < 0 || buses[index].protocol != SOLAR_OS_BUS_PROTOCOL_UART) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!buses[index].ready) {
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < SOLAR_OS_BUS_LEASE_MAX; i++) {
        if (leases[i].active && leases[i].bus_index == (size_t)index) {
            if (bus_lease != NULL) {
                *bus_lease = &leases[i];
            }
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_STATE;
}

esp_err_t solar_os_bus_uart_write(const char *name,
                                  const uint8_t *data,
                                  size_t len,
                                  size_t *written)
{
    if (written != NULL) {
        *written = 0;
    }
    if (!name_valid(name) || (data == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }
    esp_err_t ret = solar_os_buses_init();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(buses_mutex, portMAX_DELAY);
    solar_os_bus_lease_t *lease = NULL;
    ret = find_ready_uart_locked(name, &lease);
#if SOLAR_OS_PACKAGE_SERVICE_UART && SOLAR_OS_BOARD_HAS_UART
    if (ret == ESP_OK) {
        ret = solar_os_port_write(&lease->port, data, len, written);
    }
#else
    if (ret == ESP_OK) {
        ret = ESP_ERR_NOT_SUPPORTED;
    }
#endif
    xSemaphoreGive(buses_mutex);
    return ret;
}

esp_err_t solar_os_bus_uart_read(const char *name,
                                 uint8_t *data,
                                 size_t len,
                                 uint32_t timeout_ms,
                                 size_t *read_len)
{
    if (read_len != NULL) {
        *read_len = 0;
    }
    if (!name_valid(name) || (data == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }
    esp_err_t ret = solar_os_buses_init();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(buses_mutex, portMAX_DELAY);
    solar_os_bus_lease_t *lease = NULL;
    ret = find_ready_uart_locked(name, &lease);
#if SOLAR_OS_PACKAGE_SERVICE_UART && SOLAR_OS_BOARD_HAS_UART
    if (ret == ESP_OK) {
        ret = solar_os_port_read(&lease->port, data, len, timeout_ms, read_len);
    }
#else
    if (ret == ESP_OK) {
        ret = ESP_ERR_NOT_SUPPORTED;
    }
#endif
    xSemaphoreGive(buses_mutex);
    return ret;
}

esp_err_t solar_os_bus_uart_write_once(const char *name,
                                       const uint8_t *data,
                                       size_t len,
                                       size_t *written,
                                       const char *owner)
{
    esp_err_t ret = solar_os_bus_acquire(name, SOLAR_OS_BUS_PROTOCOL_UART, owner);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = solar_os_bus_uart_write(name, data, len, written);
    const esp_err_t release_ret = solar_os_bus_release(name,
                                                       SOLAR_OS_BUS_PROTOCOL_UART,
                                                       owner);
    return ret == ESP_OK ? release_ret : ret;
}

esp_err_t solar_os_bus_uart_read_once(const char *name,
                                      uint8_t *data,
                                      size_t len,
                                      uint32_t timeout_ms,
                                      size_t *read_len,
                                      const char *owner)
{
    esp_err_t ret = solar_os_bus_acquire(name, SOLAR_OS_BUS_PROTOCOL_UART, owner);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = solar_os_bus_uart_read(name, data, len, timeout_ms, read_len);
    const esp_err_t release_ret = solar_os_bus_release(name,
                                                       SOLAR_OS_BUS_PROTOCOL_UART,
                                                       owner);
    return ret == ESP_OK ? release_ret : ret;
}

#if SOLAR_OS_PACKAGE_SERVICE_ONEWIRE
static esp_err_t find_ready_onewire_pin(const char *name, int *pin)
{
    if (!name_valid(name) || pin == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_buses_init();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(buses_mutex, portMAX_DELAY);
    const int index = find_bus_index_locked(name);
    if (index < 0 || buses[index].protocol != SOLAR_OS_BUS_PROTOCOL_ONEWIRE) {
        ret = ESP_ERR_NOT_FOUND;
    } else if (!buses[index].ready || lease_count_locked((size_t)index) == 0) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        *pin = buses[index].config.onewire.pin;
        ret = ESP_OK;
    }
    xSemaphoreGive(buses_mutex);
    return ret;
}
#endif

esp_err_t solar_os_bus_onewire_reset(const char *name, bool *present)
{
    if (present == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
#if SOLAR_OS_PACKAGE_SERVICE_ONEWIRE
    int pin = -1;
    esp_err_t ret = find_ready_onewire_pin(name, &pin);
    return ret == ESP_OK
        ? solar_os_onewire_reset_configured(pin, present)
        : ret;
#else
    (void)name;
    *present = false;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t solar_os_bus_onewire_scan(const char *name,
                                    uint64_t *addresses,
                                    size_t max_addresses,
                                    size_t *address_count)
{
    if (address_count == NULL || (addresses == NULL && max_addresses > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
#if SOLAR_OS_PACKAGE_SERVICE_ONEWIRE
    int pin = -1;
    esp_err_t ret = find_ready_onewire_pin(name, &pin);
    return ret == ESP_OK
        ? solar_os_onewire_scan_configured(pin,
                                           addresses,
                                           max_addresses,
                                           address_count)
        : ret;
#else
    (void)name;
    (void)addresses;
    (void)max_addresses;
    *address_count = 0;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t solar_os_bus_onewire_transfer(const char *name,
                                        const uint8_t *tx_data,
                                        size_t tx_len,
                                        uint8_t *rx_data,
                                        size_t rx_len)
{
#if SOLAR_OS_PACKAGE_SERVICE_ONEWIRE
    int pin = -1;
    esp_err_t ret = find_ready_onewire_pin(name, &pin);
    return ret == ESP_OK
        ? solar_os_onewire_transfer_configured(pin,
                                               tx_data,
                                               tx_len,
                                               rx_data,
                                               rx_len)
        : ret;
#else
    (void)name;
    (void)tx_data;
    (void)tx_len;
    (void)rx_data;
    (void)rx_len;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t solar_os_bus_spi_add_device(const char *name,
                                      const spi_device_interface_config_t *device_config,
                                      spi_device_handle_t *device)
{
    if (!name_valid(name) || device_config == NULL || device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_buses_init();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(buses_mutex, portMAX_DELAY);
    const int index = find_bus_index_locked(name);
    if (index < 0 || buses[index].protocol != SOLAR_OS_BUS_PROTOCOL_SPI) {
        ret = ESP_ERR_NOT_FOUND;
    } else if (!buses[index].ready || lease_count_locked((size_t)index) == 0) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        ret = solar_os_routed_spi_add_device(&buses[index].config.spi,
                                             device_config,
                                             device);
    }
    xSemaphoreGive(buses_mutex);
    return ret;
}

esp_err_t solar_os_bus_spi_transfer(const char *name,
                                    int cs_pin,
                                    uint8_t mode,
                                    uint32_t speed_hz,
                                    const uint8_t *tx_data,
                                    uint8_t *rx_data,
                                    size_t len)
{
    if (!name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_buses_init();
    if (ret != ESP_OK) {
        return ret;
    }

    xSemaphoreTake(buses_mutex, portMAX_DELAY);
    const int index = find_bus_index_locked(name);
    if (index < 0 || buses[index].protocol != SOLAR_OS_BUS_PROTOCOL_SPI) {
        ret = ESP_ERR_NOT_FOUND;
    } else if (!buses[index].ready || lease_count_locked((size_t)index) == 0) {
        ret = ESP_ERR_INVALID_STATE;
    } else if (!spi_cs_allowed(&buses[index].config.spi, cs_pin)) {
        ret = ESP_ERR_INVALID_ARG;
    } else {
        ret = solar_os_routed_spi_transfer(&buses[index].config.spi,
                                           cs_pin,
                                           mode,
                                           speed_hz,
                                           tx_data,
                                           rx_data,
                                           len);
    }
    xSemaphoreGive(buses_mutex);
    return ret;
}

esp_err_t solar_os_bus_spi_transfer_once(const char *name,
                                         int cs_pin,
                                         uint8_t mode,
                                         uint32_t speed_hz,
                                         const uint8_t *tx_data,
                                         uint8_t *rx_data,
                                         size_t len,
                                         const char *owner)
{
    solar_os_bus_info_t info;
    if (!owner_valid(owner) ||
        !solar_os_bus_find(name, SOLAR_OS_BUS_PROTOCOL_SPI, &info) ||
        !spi_cs_allowed(&info.config.spi, cs_pin) ||
        mode > 3 ||
        speed_hz == 0 ||
        speed_hz > SOLAR_OS_BUS_SPI_MAX_SPEED_HZ ||
        len == 0 ||
        len > info.config.spi.max_transfer_size ||
        (tx_data == NULL && rx_data == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    const solar_os_resource_request_t requests[] = {
        {
            .kind = SOLAR_OS_RESOURCE_SPI_CS,
            .primary = cs_pin,
            .secondary = -1,
            .label = name,
        },
        {
            .kind = SOLAR_OS_RESOURCE_GPIO_PIN,
            .primary = cs_pin,
            .secondary = -1,
            .label = "spi-cs",
        },
    };
    esp_err_t ret = solar_os_resource_claim_bundle(requests,
                                                    sizeof(requests) / sizeof(requests[0]),
                                                    owner,
                                                    NULL);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = solar_os_bus_acquire(name, SOLAR_OS_BUS_PROTOCOL_SPI, owner);
    if (ret != ESP_OK) {
        (void)solar_os_resource_release_owner(owner);
        return ret;
    }
    ret = solar_os_bus_spi_transfer(name,
                                    cs_pin,
                                    mode,
                                    speed_hz,
                                    tx_data,
                                    rx_data,
                                    len);
    const esp_err_t release_ret = solar_os_bus_release(name,
                                                       SOLAR_OS_BUS_PROTOCOL_SPI,
                                                       owner);
    (void)solar_os_resource_release_owner(owner);
    return ret == ESP_OK ? release_ret : ret;
}

const char *solar_os_bus_protocol_name(solar_os_bus_protocol_t protocol)
{
    switch (protocol) {
    case SOLAR_OS_BUS_PROTOCOL_I2C:
        return "i2c";
    case SOLAR_OS_BUS_PROTOCOL_SPI:
        return "spi";
    case SOLAR_OS_BUS_PROTOCOL_UART:
        return "uart";
    case SOLAR_OS_BUS_PROTOCOL_ONEWIRE:
        return "onewire";
    default:
        return "unknown";
    }
}

const char *solar_os_bus_origin_name(solar_os_bus_origin_t origin)
{
    return origin == SOLAR_OS_BUS_ORIGIN_RUNTIME ? "runtime" : "board";
}

const char *solar_os_bus_sharing_name(solar_os_bus_sharing_t sharing)
{
    return sharing == SOLAR_OS_BUS_EXCLUSIVE ? "exclusive" : "shared";
}
