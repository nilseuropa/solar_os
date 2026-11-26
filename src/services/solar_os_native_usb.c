#include "solar_os_native_usb.h"

#include <string.h>

#include "solar_os_config.h"
#include "solar_os_native_support.h"
#include "solar_os_native_usb_abi.h"

#if SOLAR_OS_PACKAGE_SERVICE_USB_CDC
#include "solar_os_cdc.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_USB_CDC || SOLAR_OS_PACKAGE_SERVICE_USB_HOST
static int usb_cdc_status(solar_os_cdc_status_t *status)
{
    if (!status) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
#if SOLAR_OS_PACKAGE_SERVICE_USB_CDC
    solar_os_cdc_get_status(status);
    return SOLAR_OS_NATIVE_OK;
#else
    *status = (solar_os_cdc_status_t){0};
    return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
#endif
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_USB_CDC
static const solar_os_native_usb_cdc_api_v1_t usb_cdc_api = {
    .abi_version = SOLAR_OS_NATIVE_USB_CDC_ABI,
    .struct_size = sizeof(usb_cdc_api),
    .status = usb_cdc_status,
};
#endif

#if SOLAR_OS_PACKAGE_SERVICE_USB_HOST
#include "solar_os_board_caps.h"
#include "solar_os_usb.h"
#if SOLAR_OS_PACKAGE_SERVICE_USB_STORAGE
#include "solar_os_usb_storage.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_USB_SERIAL
#include "solar_os_usb_serial.h"
#endif

static int usb_set_mode(solar_os_usb_mode_t mode)
{
    return solar_os_native_result(solar_os_usb_set_mode(mode));
}

static int usb_status(solar_os_usb_status_t *status)
{
    if (!status) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_usb_get_status(status);
    status->last_error = solar_os_native_result(status->last_error);
    status->rollback_error = solar_os_native_result(status->rollback_error);
    for (size_t i = 0; i < status->interface_count; i++) {
        solar_os_usb_interface_status_t *item = &status->interfaces[i];
        item->protocol_error = solar_os_native_result(item->protocol_error);
        item->idle_error = solar_os_native_result(item->idle_error);
        item->last_error = solar_os_native_result(item->last_error);
    }
    return SOLAR_OS_NATIVE_OK;
}

static int usb_storage_status(solar_os_usb_storage_status_t *status)
{
    if (!status) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
#if SOLAR_OS_PACKAGE_SERVICE_USB_STORAGE
    solar_os_usb_storage_get_status(status);
    status->last_error = solar_os_native_result(status->last_error);
    return SOLAR_OS_NATIVE_OK;
#else
    *status = (solar_os_usb_storage_status_t){0};
    return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
#endif
}

#if SOLAR_OS_PACKAGE_SERVICE_USB_SERIAL
static int usb_serial_snapshot(solar_os_native_usb_serial_info_v1_t *ports, size_t capacity,
                               size_t *total, int *last_error)
{
    if (total) *total = 0;
    if (last_error) *last_error = SOLAR_OS_NATIVE_OK;
    if (!total || (capacity && !ports)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_usb_serial_info_t snapshot[SOLAR_OS_USB_SERIAL_MAX];
    if (capacity > SOLAR_OS_USB_SERIAL_MAX) capacity = SOLAR_OS_USB_SERIAL_MAX;
    *total = solar_os_usb_serial_list(capacity ? snapshot : NULL, capacity);
    const size_t count = *total < capacity ? *total : capacity;
    for (size_t i = 0; i < count; ++i) {
        const solar_os_usb_serial_info_t *from = &snapshot[i];
        solar_os_native_usb_serial_info_v1_t *to = &ports[i];
        *to = (solar_os_native_usb_serial_info_v1_t){
            .vendor_id = from->device.vid,
            .product_id = from->device.pid,
            .address = from->device.address,
            .interface = from->device.interface,
            .kind = from->device.kind,
            .config = {
                .baud_rate = from->config.baud_rate, .data_bits = from->config.data_bits,
                .parity = from->config.parity, .stop_bits = from->config.stop_bits,
                .dtr = from->config.dtr, .rts = from->config.rts,
            },
            .connected = from->connected,
            .received = from->received,
            .transmitted = from->transmitted,
            .dropped = from->dropped,
            .last_error = solar_os_native_result(from->last_error),
        };
        _Static_assert(sizeof(to->port) == sizeof(from->port), "USB serial port name capacity");
        memcpy(to->port, from->port, sizeof(to->port));
    }
    if (last_error) *last_error = solar_os_native_result(solar_os_usb_serial_last_error());
    return SOLAR_OS_NATIVE_OK;
}

static const solar_os_native_usb_serial_api_v1_t usb_serial_api = {
    .abi_version = SOLAR_OS_NATIVE_USB_SERIAL_ABI,
    .struct_size = sizeof(usb_serial_api),
    .snapshot = usb_serial_snapshot,
};
#endif

static const solar_os_native_usb_api_v1_t usb_api = {
    .abi_version = SOLAR_OS_NATIVE_USB_ABI,
    .struct_size = sizeof(usb_api),
    .capabilities = 0
#if SOLAR_OS_PACKAGE_SERVICE_USB_CDC
        | SOLAR_OS_NATIVE_USB_CAP_CDC
#endif
#if SOLAR_OS_PACKAGE_SERVICE_USB_STORAGE
        | SOLAR_OS_NATIVE_USB_CAP_STORAGE
#endif
#if SOLAR_OS_PACKAGE_SERVICE_USB_SERIAL
        | SOLAR_OS_NATIVE_USB_CAP_SERIAL
#endif
#ifdef SOLAR_OS_BOARD_PIN_USB_VBUS_EN
        | SOLAR_OS_NATIVE_USB_CAP_VBUS_SWITCH
#endif
        ,
    .set_mode = usb_set_mode,
    .status = usb_status,
    .mode_name = solar_os_usb_mode_name,
    .cdc_status = usb_cdc_status,
    .storage_status = usb_storage_status,
};
#endif

const void *solar_os_native_usb_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_usb_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
