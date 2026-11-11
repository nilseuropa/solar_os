#pragma once

#include "solar_os_port.h"

typedef enum {
    SOLAR_OS_USB_SERIAL_ACM,
    SOLAR_OS_USB_SERIAL_JTAG,
    SOLAR_OS_USB_SERIAL_CP210X,
    SOLAR_OS_USB_SERIAL_CH34X,
} usb_serial_port_kind_t;

typedef struct {
    uint16_t vid, pid;
    uint8_t address, interface;
    usb_serial_port_kind_t kind;
} usb_serial_port_candidate_t;

typedef void (*usb_serial_port_discovery_fn)(const usb_serial_port_candidate_t *candidate);
typedef void (*usb_serial_port_rx_fn)(const uint8_t *data, size_t length, void *user);
typedef void (*usb_serial_port_event_fn)(esp_err_t error, bool disconnected, void *user);

typedef struct {
    void *handle, *user;
    usb_serial_port_kind_t kind;
    usb_serial_port_rx_fn receive;
    usb_serial_port_event_fn event;
} usb_serial_port_device_t;

/* The OS USB host library must already be installed. Callbacks only queue
 * work or copy data; opening/closing happens in the serial service worker. */
esp_err_t usb_serial_port_start(usb_serial_port_discovery_fn discovered);
esp_err_t usb_serial_port_stop(void);
esp_err_t usb_serial_port_open(const usb_serial_port_candidate_t *candidate,
    usb_serial_port_device_t *device, usb_serial_port_rx_fn receive,
    usb_serial_port_event_fn event, void *user);
esp_err_t usb_serial_port_close(usb_serial_port_device_t *device);
esp_err_t usb_serial_port_write(usb_serial_port_device_t *device, const uint8_t *data, size_t length);
esp_err_t usb_serial_port_line(usb_serial_port_device_t *device, const solar_os_port_serial_config_t *config);
esp_err_t usb_serial_port_lines(usb_serial_port_device_t *device, bool dtr, bool rts);
esp_err_t usb_serial_port_break(usb_serial_port_device_t *device, uint16_t duration_ms);
const char *usb_serial_port_kind_name(usb_serial_port_kind_t kind);
