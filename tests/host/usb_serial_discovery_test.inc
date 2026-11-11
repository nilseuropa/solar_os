#include <assert.h>
#include <string.h>
#include "usb_serial_port.h"
#define USB_B_DESCRIPTOR_TYPE_INTERFACE 4
#define __packed __attribute__((packed))
typedef void *usb_device_handle_t;
typedef struct { uint16_t idVendor, idProduct; } usb_device_desc_t;
typedef struct __packed { uint8_t bLength, bDescriptorType; uint16_t wTotalLength; uint8_t rest[5]; } usb_config_desc_t;
typedef struct __packed { uint8_t bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting, bNumEndpoints, bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface; } usb_intf_desc_t;
typedef struct { uint8_t dev_addr; } usb_device_info_t;
static usb_device_desc_t device;
static uint8_t address;
static unsigned count;
static uint8_t raw[] = {
    9, 2, 36, 0, 3, 1, 0, 0x80, 50,
    9, 4, 0, 0, 1, 2, 2, 1, 0, /* Leonardo CDC communication */
    9, 4, 1, 0, 2, 10, 0, 0, 0, /* CDC data */
    9, 4, 2, 0, 1, 3, 0, 0, 0, /* HID keyboard */
};
static usb_serial_port_candidate_t found[8];
static void discovery(const usb_serial_port_candidate_t *candidate) { assert(count < 8); found[count++] = *candidate; }
static esp_err_t usb_host_get_device_descriptor(void *h, const usb_device_desc_t **out)
{ assert(h); *out = &device; return ESP_OK; }
static esp_err_t usb_host_get_active_config_descriptor(void *h, const usb_config_desc_t **out)
{ assert(h); *out = (usb_config_desc_t *)raw; return ESP_OK; }
static esp_err_t usb_host_device_info(void *h, usb_device_info_t *out)
{ assert(h); out->dev_addr = address; return ESP_OK; }
