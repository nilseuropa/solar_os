#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_board_caps.h"
#include "solar_os_expansion_types.h"

#define SOLAR_OS_EXPANSION_DRIVER_NAME_MAX 20
#define SOLAR_OS_EXPANSION_DEVICE_NAME_MAX 20
#define SOLAR_OS_EXPANSION_ROLE_MAX 16
#define SOLAR_OS_EXPANSION_TARGET_MAX 16
#define SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX 8

typedef enum {
    SOLAR_OS_EXPANSION_BINDING_GPIO,
    SOLAR_OS_EXPANSION_BINDING_ADC,
    SOLAR_OS_EXPANSION_BINDING_PWM,
    SOLAR_OS_EXPANSION_BINDING_I2C_BUS,
    SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS,
    SOLAR_OS_EXPANSION_BINDING_SPI_BUS,
    SOLAR_OS_EXPANSION_BINDING_SPI_CS,
    SOLAR_OS_EXPANSION_BINDING_UART_PORT,
} solar_os_expansion_binding_kind_t;

typedef struct {
    solar_os_expansion_binding_kind_t kind;
    char role[SOLAR_OS_EXPANSION_ROLE_MAX];
    char target[SOLAR_OS_EXPANSION_TARGET_MAX];
    int value;
    int aux;
} solar_os_expansion_binding_t;

typedef esp_err_t (*solar_os_expansion_attach_fn_t)(const char *name,
                                                    const solar_os_expansion_binding_t *bindings,
                                                    size_t binding_count);
typedef esp_err_t (*solar_os_expansion_detach_fn_t)(const char *name);

typedef struct {
    const char *name;
    const char *summary;
    solar_os_board_capabilities_t required_capabilities;
    bool probe_supported;
    solar_os_expansion_attach_fn_t attach;
    solar_os_expansion_detach_fn_t detach;
} solar_os_expansion_driver_t;

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char driver[SOLAR_OS_EXPANSION_DRIVER_NAME_MAX];
    size_t binding_count;
    solar_os_expansion_binding_t bindings[SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX];
} solar_os_expansion_device_t;

esp_err_t solar_os_expansion_init(void);
bool solar_os_expansion_available(void);

size_t solar_os_expansion_driver_count(void);
bool solar_os_expansion_get_driver(size_t index, solar_os_expansion_driver_t *driver);
bool solar_os_expansion_driver_supported(const char *name);

size_t solar_os_expansion_i2c_bus_count(void);
bool solar_os_expansion_get_i2c_bus(size_t index, solar_os_expansion_i2c_bus_t *bus);
bool solar_os_expansion_find_i2c_bus(const char *name, solar_os_expansion_i2c_bus_t *bus, size_t *index);

size_t solar_os_expansion_spi_bus_count(void);
bool solar_os_expansion_get_spi_bus(size_t index, solar_os_expansion_spi_bus_t *bus);
bool solar_os_expansion_find_spi_bus(const char *name, solar_os_expansion_spi_bus_t *bus, size_t *index);
bool solar_os_expansion_spi_cs_allowed(const char *bus_name, int pin);

size_t solar_os_expansion_uart_port_count(void);
bool solar_os_expansion_get_uart_port(size_t index, solar_os_expansion_uart_port_t *port);
bool solar_os_expansion_find_uart_port(const char *name, solar_os_expansion_uart_port_t *port, size_t *index);

size_t solar_os_expansion_onewire_bus_count(void);
bool solar_os_expansion_get_onewire_bus(size_t index, solar_os_expansion_onewire_bus_t *bus);
bool solar_os_expansion_find_onewire_bus(const char *name,
                                         solar_os_expansion_onewire_bus_t *bus,
                                         size_t *index);

esp_err_t solar_os_expansion_attach(const char *driver,
                                    const char *name,
                                    const solar_os_expansion_binding_t *bindings,
                                    size_t binding_count);
esp_err_t solar_os_expansion_detach(const char *name);
size_t solar_os_expansion_device_count(void);
/* Returns a caller-owned snapshot; no registry pointer is borrowed. */
bool solar_os_expansion_get_device(size_t index, solar_os_expansion_device_t *device);

const char *solar_os_expansion_binding_kind_name(solar_os_expansion_binding_kind_t kind);
