#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "usb_hid_mouse_report.h"

#define USB_HOST_PORT_INTERFACE_MAX 8U

typedef struct {
    uint8_t slot;
    uint8_t address;
    uint8_t interface;
    uint8_t protocol;
    uint16_t vendor_id;
    uint16_t product_id;
    /* 0=boot, 1=report, 0xff=GET_PROTOCOL unavailable. */
    uint8_t report_protocol;
    esp_err_t protocol_error;
    esp_err_t idle_error;
    esp_err_t last_error;
    solar_os_usb_hid_mouse_layout_t mouse_layout;
    bool ready;
    uint32_t reports;
    uint32_t errors;
} usb_host_port_interface_t;

typedef enum {
    USB_HOST_PORT_OPEN,
    USB_HOST_PORT_REPORT,
    USB_HOST_PORT_RESET,
    USB_HOST_PORT_CLOSE,
} usb_host_port_event_t;

/* Called at task level, serialized by the driver. Keep the callback bounded;
 * it must not call USB lifecycle or enumeration APIs. */
typedef esp_err_t (*usb_host_port_callback_t)(usb_host_port_event_t event,
    const usb_host_port_interface_t *interface, const uint8_t *data, size_t length);

esp_err_t usb_host_port_start(usb_host_port_callback_t callback);
esp_err_t usb_host_port_stop(void);
bool usb_host_port_is_active(void);
size_t usb_host_port_interfaces(usb_host_port_interface_t *interfaces, size_t capacity);
