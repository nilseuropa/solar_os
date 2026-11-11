#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_PORT_MAX 8
#define SOLAR_OS_PORT_NAME_MAX 16
#define SOLAR_OS_PORT_LABEL_MAX 32
#define SOLAR_OS_PORT_OWNER_MAX 24

typedef enum {
    SOLAR_OS_PORT_CAP_READ = 1U << 0,
    SOLAR_OS_PORT_CAP_WRITE = 1U << 1,
    SOLAR_OS_PORT_CAP_CONFIG = 1U << 2,
    SOLAR_OS_PORT_CAP_SERIAL = 1U << 3,
    SOLAR_OS_PORT_CAP_USB_JTAG = 1U << 4, /* Native Espressif USB reset sequence. */
} solar_os_port_capability_t;

/* USB CDC line coding: parity 0=none, 1=odd, 2=even, 3=mark, 4=space;
 * stop_bits 0=one, 1=one-and-a-half, 2=two. */
typedef struct {
    uint32_t baud_rate;
    uint8_t data_bits, parity, stop_bits;
    bool dtr, rts;
} solar_os_port_serial_config_t;

enum {
    SOLAR_OS_SERIAL_BAUD = 1U << 0,
    SOLAR_OS_SERIAL_BITS = 1U << 1,
    SOLAR_OS_SERIAL_PARITY = 1U << 2,
    SOLAR_OS_SERIAL_STOP = 1U << 3,
    SOLAR_OS_SERIAL_DTR = 1U << 4,
    SOLAR_OS_SERIAL_RTS = 1U << 5,
    SOLAR_OS_SERIAL_ALL = (1U << 6) - 1,
};
typedef enum {
    SOLAR_OS_PORT_SERIAL_GET,
    SOLAR_OS_PORT_SERIAL_UPDATE,
    SOLAR_OS_PORT_SERIAL_BREAK,
} solar_os_port_control_t;
typedef struct {
    solar_os_port_serial_config_t config;
    uint32_t mask;
} solar_os_port_serial_update_t;
typedef esp_err_t (*solar_os_port_control_fn)(void *user, solar_os_port_control_t request, void *data);

typedef struct {
    int index;
    uint32_t token;
} solar_os_port_handle_t;

#define SOLAR_OS_PORT_HANDLE_INIT { .index = -1, .token = 0 }

typedef esp_err_t (*solar_os_port_read_fn)(void *user,
                                           uint8_t *data,
                                           size_t len,
                                           uint32_t timeout_ms,
                                           size_t *read_len);
typedef esp_err_t (*solar_os_port_write_fn)(void *user,
                                            const uint8_t *data,
                                            size_t len,
                                            size_t *written);
typedef esp_err_t (*solar_os_port_open_fn)(void *user);
typedef esp_err_t (*solar_os_port_close_fn)(void *user);

typedef struct {
    const char *name;
    const char *label;
    uint32_t capabilities;
    solar_os_port_read_fn read;
    solar_os_port_write_fn write;
    solar_os_port_open_fn open;
    solar_os_port_close_fn close;
    void *user;
    solar_os_port_control_fn control;
} solar_os_port_driver_t;

typedef struct {
    char name[SOLAR_OS_PORT_NAME_MAX];
    char label[SOLAR_OS_PORT_LABEL_MAX];
    uint32_t capabilities;
    bool claimed;
    unsigned operations;
    char owner[SOLAR_OS_PORT_OWNER_MAX];
} solar_os_port_info_t;

esp_err_t solar_os_port_init(void);
esp_err_t solar_os_port_register(const solar_os_port_driver_t *driver);
esp_err_t solar_os_port_unregister(const char *name);
/* Atomic retirement: busy ports/streams leave the entire set registered. */
esp_err_t solar_os_port_unregister_many(const char *const *names, size_t count);
esp_err_t solar_os_port_claim(const char *name,
                              const char *owner,
                              solar_os_port_handle_t *handle);
esp_err_t solar_os_port_release(solar_os_port_handle_t *handle);
bool solar_os_port_handle_valid(const solar_os_port_handle_t *handle);
esp_err_t solar_os_port_read(const solar_os_port_handle_t *handle,
                             uint8_t *data,
                             size_t len,
                             uint32_t timeout_ms,
                             size_t *read_len);
esp_err_t solar_os_port_write(const solar_os_port_handle_t *handle,
                              const uint8_t *data,
                              size_t len,
                              size_t *written);
esp_err_t solar_os_port_get_info(const char *name, solar_os_port_info_t *info);
size_t solar_os_port_list(solar_os_port_info_t *ports, size_t max_ports);
esp_err_t solar_os_port_serial_get(const solar_os_port_handle_t *handle, solar_os_port_serial_config_t *config);
esp_err_t solar_os_port_serial_configure(const solar_os_port_handle_t *handle,
    const solar_os_port_serial_config_t *config, uint32_t mask);
esp_err_t solar_os_port_serial_break(const solar_os_port_handle_t *handle, uint16_t duration_ms);
const char *solar_os_port_capabilities_text(uint32_t capabilities,
                                            char *buffer,
                                            size_t buffer_len);
