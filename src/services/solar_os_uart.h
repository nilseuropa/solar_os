#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_bus_types.h"
#include "solar_os_native_bus_abi.h"
#include "solar_os_port.h"

#define SOLAR_OS_UART_DEFAULT_BAUD_RATE SOLAR_OS_BUS_UART_DEFAULT_BAUD_RATE
#define SOLAR_OS_UART_MIN_BAUD_RATE SOLAR_OS_BUS_UART_MIN_BAUD_RATE
#define SOLAR_OS_UART_MAX_BAUD_RATE SOLAR_OS_BUS_UART_MAX_BAUD_RATE
#define SOLAR_OS_UART_PORT_NAME SOLAR_OS_NATIVE_BUS_DEFAULT_UART

typedef struct {
    uint32_t baud_rate;
    uint32_t measured_baud_rate;
    uint32_t edge_count;
} solar_os_uart_autobaud_result_t;

esp_err_t solar_os_uart_init(void);
esp_err_t solar_os_uart_register_bus(const char *name,
                                     const solar_os_bus_uart_config_t *config,
                                     bool persistent);
esp_err_t solar_os_uart_unregister_bus(const char *name);
esp_err_t solar_os_uart_bus_attach(const char *name);
esp_err_t solar_os_uart_bus_detach(const char *name);
bool solar_os_uart_is_valid_baud_rate(uint32_t baud_rate);
esp_err_t solar_os_uart_bus_set_baud_rate(const char *name, uint32_t baud_rate);
esp_err_t solar_os_uart_bus_set_baud_rate_owned(const char *name,
                                                uint32_t baud_rate,
                                                const char *owner);
esp_err_t solar_os_uart_bus_autobaud_start(const char *name);
esp_err_t solar_os_uart_bus_autobaud_finish(const char *name,
                                            solar_os_uart_autobaud_result_t *result);
esp_err_t solar_os_uart_bus_autobaud_cancel(const char *name);
esp_err_t solar_os_uart_bus_set_mode(const char *name, solar_os_uart_mode_t mode);
esp_err_t solar_os_uart_bus_set_mode_owned(const char *name, solar_os_uart_mode_t mode,
                                          const char *owner);
esp_err_t solar_os_uart_set_baud_rate(uint32_t baud_rate);
esp_err_t solar_os_uart_set_mode(solar_os_uart_mode_t mode);
esp_err_t solar_os_uart_bus_write(const char *name,
                                  const uint8_t *data,
                                  size_t len,
                                  size_t *written);
esp_err_t solar_os_uart_bus_read(const char *name,
                                 uint8_t *data,
                                 size_t len,
                                 uint32_t timeout_ms,
                                 size_t *read_len);
esp_err_t solar_os_uart_write(const uint8_t *data, size_t len, size_t *written);
esp_err_t solar_os_uart_read(uint8_t *data, size_t len, uint32_t timeout_ms, size_t *read_len);
bool solar_os_uart_get_bus_status(const char *name, solar_os_uart_status_t *status);
esp_err_t solar_os_uart_copy_bus_status(const char *name, solar_os_uart_status_t *status);
void solar_os_uart_get_status(solar_os_uart_status_t *status);
const char *solar_os_uart_mode_name(solar_os_uart_mode_t mode);
bool solar_os_uart_parse_mode(const char *text, solar_os_uart_mode_t *mode);
