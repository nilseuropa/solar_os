#pragma once

#include "usb_serial_port.h"

#define SOLAR_OS_USB_SERIAL_MAX 4U

typedef struct {
    char port[SOLAR_OS_PORT_NAME_MAX];
    usb_serial_port_candidate_t device;
    solar_os_port_serial_config_t config;
    bool connected;
    uint32_t received, transmitted, dropped;
    esp_err_t last_error;
} solar_os_usb_serial_info_t;

esp_err_t solar_os_usb_serial_start(void);
esp_err_t solar_os_usb_serial_prepare_stop(void);
void solar_os_usb_serial_cancel_stop(void);
esp_err_t solar_os_usb_serial_stop(void);
size_t solar_os_usb_serial_list(solar_os_usb_serial_info_t *info, size_t capacity);
esp_err_t solar_os_usb_serial_last_error(void);
/* A short exclusive configuration lease, for shell/scripts before opening a
 * byte stream. Already claimed ports must be configured through their handle. */
esp_err_t solar_os_usb_serial_config(const char *name, solar_os_port_serial_config_t *config, uint32_t mask);
esp_err_t solar_os_usb_serial_send_break(const char *name, uint16_t duration_ms);
esp_err_t solar_os_usb_serial_parse_option(const char *key, const char *value,
    solar_os_port_serial_config_t *config, uint32_t *mask);
