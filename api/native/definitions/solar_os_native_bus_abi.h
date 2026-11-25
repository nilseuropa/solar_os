#pragma once
#include "solar_os_native_service_abi.h"
#define SOLAR_OS_NATIVE_BUS_SERVICE "bus"
#define SOLAR_OS_NATIVE_BUS_ABI 1U
#define SOLAR_OS_NATIVE_BUS_I2C 0U
#define SOLAR_OS_NATIVE_BUS_SPI 1U
#define SOLAR_OS_NATIVE_BUS_UART 2U
#define SOLAR_OS_NATIVE_BUS_ONEWIRE 3U
#define SOLAR_OS_NATIVE_BUS_PS2 4U
#define SOLAR_OS_NATIVE_BUS_MIDI 5U
#define SOLAR_OS_NATIVE_BUS_BOARD 0U
#define SOLAR_OS_NATIVE_BUS_RUNTIME 1U
#define SOLAR_OS_NATIVE_BUS_SHARED 0U
#define SOLAR_OS_NATIVE_BUS_EXCLUSIVE 1U
#define SOLAR_OS_NATIVE_BUS_ALL (-1)
typedef struct { char name[16]; int32_t pin; } solar_os_native_bus_cs_v1_t;
typedef struct {
    int32_t port, sda_pin, scl_pin, host, sclk_pin, miso_pin, mosi_pin, tx_pin, rx_pin, onewire_pin, ps2_clock_pin, ps2_data_pin;
    uint32_t speed_hz, baud_rate, max_transfer_size, cs_count;
    solar_os_native_bus_cs_v1_t cs[5];
} solar_os_native_bus_config_v1_t;
typedef struct {
    char name[16]; uint32_t protocol, origin, sharing;
    solar_os_native_bus_config_v1_t config;
} solar_os_native_bus_definition_v1_t;
typedef struct {
    solar_os_native_bus_definition_v1_t definition;
    uint64_t id, lease_count;
    uint32_t active, attached, detachable, ready;
} solar_os_native_bus_info_v1_t;
typedef struct { uint32_t baud_rate, measured_baud_rate, edge_count; } solar_os_native_autobaud_v1_t;
/* Definition records use explicit fields rather than a backend union. The OS
 * owns copied definitions; leases use unique owner keys and explicit close.
 * Transfer buffers are borrowed only during a call. A lease is serialized by
 * its caller. Protocol-specific operations reject a lease of another protocol.
 * I2C uses 7-bit addresses; registers are 8-bit. SPI mode is 0..3. */
typedef struct {
    uint32_t abi_version, struct_size;
    bool (*runtime_available)(uint32_t protocol);
    size_t (*endpoint_count)(uint32_t protocol);
    int (*endpoint)(uint32_t protocol, size_t index, int32_t *endpoint);
    int (*register_bus)(const solar_os_native_bus_definition_v1_t *definition);
    int (*unregister_bus)(const char *name);
    int (*attach)(const char *name);
    int (*detach)(const char *name);
    size_t (*count)(int32_t protocol);
    int (*info)(size_t index, int32_t protocol, solar_os_native_bus_info_v1_t *info);
    int (*find)(const char *name, uint32_t protocol, solar_os_native_bus_info_v1_t *info);
    int (*acquire)(const char *name, uint32_t protocol, const char *owner, solar_os_native_handle_t *lease);
    int (*close)(solar_os_native_handle_t lease);
    int (*i2c_speed)(solar_os_native_handle_t lease, uint32_t speed_hz);
    int (*i2c_probe)(solar_os_native_handle_t lease, uint32_t address);
    int (*i2c_read)(solar_os_native_handle_t lease, uint32_t address, void *data, size_t size);
    int (*i2c_write)(solar_os_native_handle_t lease, uint32_t address, const void *data, size_t size);
    int (*i2c_exchange)(solar_os_native_handle_t lease, uint32_t address, const void *tx, size_t tx_size, void *rx, size_t rx_size);
    int (*i2c_read_reg)(solar_os_native_handle_t lease, uint32_t address, uint32_t reg, void *data, size_t size);
    int (*i2c_write_reg)(solar_os_native_handle_t lease, uint32_t address, uint32_t reg, const void *data, size_t size);
    int (*uart_read)(solar_os_native_handle_t lease, void *data, size_t size, uint32_t timeout_ms, size_t *read_size);
    int (*uart_write)(solar_os_native_handle_t lease, const void *data, size_t size, size_t *written);
    int (*uart_baud)(solar_os_native_handle_t lease, uint32_t baud_rate);
    int (*autobaud_start)(solar_os_native_handle_t lease);
    int (*autobaud_finish)(solar_os_native_handle_t lease, solar_os_native_autobaud_v1_t *result);
    int (*autobaud_cancel)(solar_os_native_handle_t lease);
    int (*onewire_reset)(solar_os_native_handle_t lease, bool *present);
    int (*onewire_scan)(solar_os_native_handle_t lease, uint64_t *addresses, size_t capacity, size_t *count);
    int (*onewire_exchange)(solar_os_native_handle_t lease, const void *tx, size_t tx_size, void *rx, size_t rx_size);
    int (*spi_exchange)(solar_os_native_handle_t lease, int32_t cs_pin, uint32_t mode, uint32_t speed_hz, const void *tx, void *rx, size_t size);
} solar_os_native_bus_api_v1_t;
