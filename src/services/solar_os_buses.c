#include "solar_os_buses.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_board.h"
#include "solar_os_expansion_types.h"
#include "solar_os_pins.h"

#define SOLAR_OS_BUS_MAX 8
#define SOLAR_OS_BUS_LEASE_MAX 16

typedef struct {
    bool active;
    size_t bus_index;
    char owner[SOLAR_OS_BUS_OWNER_MAX];
    size_t ref_count;
} solar_os_bus_lease_t;

static solar_os_bus_info_t buses[SOLAR_OS_BUS_MAX];
static solar_os_bus_lease_t leases[SOLAR_OS_BUS_LEASE_MAX];
static SemaphoreHandle_t buses_mutex;
static StaticSemaphore_t buses_mutex_buffer;
static bool buses_initialized;

static const solar_os_expansion_i2c_bus_t board_i2c_buses[] =
    SOLAR_OS_BOARD_EXPANSION_I2C_BUSES;
static const solar_os_expansion_spi_bus_t board_spi_buses[] =
    SOLAR_OS_BOARD_EXPANSION_SPI_BUSES;
static const solar_os_expansion_uart_port_t board_uart_buses[] =
    SOLAR_OS_BOARD_EXPANSION_UART_PORTS;

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
            definition->config.i2c.sda_pin >= 0 &&
            definition->config.i2c.scl_pin >= 0 &&
            definition->config.i2c.sda_pin != definition->config.i2c.scl_pin;
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
            definition->config.spi.cs_count <= SOLAR_OS_BUS_SPI_CS_MAX;
        for (size_t i = 0; config_valid && i < definition->config.spi.cs_count; i++) {
            config_valid = name_valid(definition->config.spi.cs[i].name) &&
                definition->config.spi.cs[i].pin >= 0;
        }
        break;
    case SOLAR_OS_BUS_PROTOCOL_UART:
        config_valid = definition->config.uart.port >= 0 &&
            definition->config.uart.tx_pin >= 0 &&
            definition->config.uart.rx_pin >= 0 &&
            definition->config.uart.tx_pin != definition->config.uart.rx_pin;
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
        if (!solar_os_pin_is_routable(definition->config.spi.sclk_pin) ||
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
        return solar_os_pin_is_routable(definition->config.uart.tx_pin) &&
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

static esp_err_t register_board_i2c_locked(const solar_os_expansion_i2c_bus_t *bus)
{
    if (bus == NULL || bus->name[0] == '\0') {
        return ESP_OK;
    }
    const solar_os_bus_definition_t definition = {
        .name = bus->name,
        .protocol = SOLAR_OS_BUS_PROTOCOL_I2C,
        .origin = SOLAR_OS_BUS_ORIGIN_BOARD,
        .sharing = SOLAR_OS_BUS_SHARED,
        .config.i2c = {
            .port = bus->port,
            .sda_pin = bus->sda_pin,
            .scl_pin = bus->scl_pin,
        },
    };
    return register_locked(&definition);
}

static esp_err_t register_board_spi_locked(const solar_os_expansion_spi_bus_t *bus)
{
    if (bus == NULL || bus->name[0] == '\0') {
        return ESP_OK;
    }
    solar_os_bus_definition_t definition = {
        .name = bus->name,
        .protocol = SOLAR_OS_BUS_PROTOCOL_SPI,
        .origin = SOLAR_OS_BUS_ORIGIN_BOARD,
        .sharing = SOLAR_OS_BUS_SHARED,
        .config.spi = {
            .host = bus->host,
            .sclk_pin = bus->sclk_pin,
            .miso_pin = bus->miso_pin,
            .mosi_pin = bus->mosi_pin,
            .max_transfer_size = bus->max_transfer_size,
            .cs_count = bus->cs_count,
        },
    };
    for (size_t i = 0; i < bus->cs_count && i < SOLAR_OS_BUS_SPI_CS_MAX; i++) {
        definition.config.spi.cs[i] = bus->cs[i];
    }
    return register_locked(&definition);
}

static esp_err_t register_board_uart_locked(const solar_os_expansion_uart_port_t *bus)
{
    if (bus == NULL || bus->name[0] == '\0') {
        return ESP_OK;
    }
    const solar_os_bus_definition_t definition = {
        .name = bus->name,
        .protocol = SOLAR_OS_BUS_PROTOCOL_UART,
        .origin = SOLAR_OS_BUS_ORIGIN_BOARD,
        .sharing = SOLAR_OS_BUS_EXCLUSIVE,
        .config.uart = {
            .port = bus->port,
            .tx_pin = bus->tx_pin,
            .rx_pin = bus->rx_pin,
        },
    };
    return register_locked(&definition);
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

    for (size_t i = 0; ret == ESP_OK && i < sizeof(board_i2c_buses) / sizeof(board_i2c_buses[0]); i++) {
        ret = register_board_i2c_locked(&board_i2c_buses[i]);
    }
    for (size_t i = 0; ret == ESP_OK && i < sizeof(board_spi_buses) / sizeof(board_spi_buses[0]); i++) {
        ret = register_board_spi_locked(&board_spi_buses[i]);
    }
    for (size_t i = 0; ret == ESP_OK && i < sizeof(board_uart_buses) / sizeof(board_uart_buses[0]); i++) {
        ret = register_board_uart_locked(&board_uart_buses[i]);
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

    xSemaphoreTake(buses_mutex, portMAX_DELAY);
    ret = register_locked(definition);
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
        memset(&buses[index], 0, sizeof(buses[index]));
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
    *free_lease = (solar_os_bus_lease_t) {
        .active = true,
        .bus_index = (size_t)bus_index,
        .ref_count = 1,
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
        released += leases[i].ref_count;
        memset(&leases[i], 0, sizeof(leases[i]));
    }
    xSemaphoreGive(buses_mutex);
    return released;
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
