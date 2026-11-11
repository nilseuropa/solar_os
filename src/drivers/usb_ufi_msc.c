#include "usb_ufi_msc.h"

#include <string.h>
#include "msc_common.h"
#include "freertos/task.h"

/* Single directly connected drive. All access is serialized by storage;
 * driver callbacks only change USB presence, never this protocol state. */
static struct {
    msc_device_t *device;
    solar_os_usb_ufi_config_t config;
    solar_os_usb_ufi_t ufi;
} cbi;

static esp_err_t adsc(void *context, const uint8_t command[12])
{
    msc_device_t *device = context;
    usb_setup_packet_t *setup = (usb_setup_packet_t *)device->xfer->data_buffer;
    *setup = (usb_setup_packet_t){.bmRequestType = 0x21, .bRequest = 0,
        .wValue = 0, .wIndex = device->config.iface_num, .wLength = 12};
    memcpy(device->xfer->data_buffer + USB_SETUP_PACKET_SIZE, command, 12);
    esp_err_t err = msc_control_transfer(device, USB_SETUP_PACKET_SIZE + 12);
    if (device->xfer->status == USB_TRANSFER_STATUS_STALL) return SOLAR_OS_UFI_STALL;
    if (err == ESP_OK && device->xfer->actual_num_bytes != USB_SETUP_PACKET_SIZE + 12)
        return ESP_ERR_INVALID_RESPONSE;
    return err;
}

static esp_err_t data_transfer(void *context, bool input, void *data, size_t size, size_t *actual)
{
    esp_err_t err = solar_os_usb_ufi_msc_endpoint(context,
        input ? cbi.config.bulk_in : cbi.config.bulk_out, cbi.config.bulk_in_mps, data, size, actual);
    return err == ESP_ERR_MSC_STALL ? SOLAR_OS_UFI_STALL : err;
}
static esp_err_t status_transfer(void *context, uint8_t status[2], size_t *actual)
{
    return solar_os_usb_ufi_msc_endpoint(context, cbi.config.interrupt_in,
        cbi.config.interrupt_mps, status, 2, actual);
}
static esp_err_t clear(void *context, bool input)
{ return clear_feature(context, input ? cbi.config.bulk_in : cbi.config.bulk_out); }
static void delay(void *context, unsigned milliseconds)
{ (void)context; vTaskDelay(pdMS_TO_TICKS(milliseconds)); }

esp_err_t solar_os_usb_ufi_msc_bind(void *context)
{
    msc_device_t *device = context;
    const usb_config_desc_t *descriptor;
    esp_err_t err = usb_host_get_active_config_descriptor(device->handle, &descriptor);
    if (err != ESP_OK) return err;
    solar_os_usb_ufi_config_t config;
    err = solar_os_usb_ufi_parse_config((const uint8_t *)descriptor, descriptor->wTotalLength,
        device->config.iface_num, &config);
    if (err != ESP_OK) return err;
    if (cbi.device) return ESP_ERR_INVALID_STATE;
    memset(&cbi, 0, sizeof(cbi)); cbi.device = device; cbi.config = config;
    device->config.bulk_in_ep = config.bulk_in; device->config.bulk_in_mps = config.bulk_in_mps;
    device->config.bulk_out_ep = config.bulk_out;
    return ESP_OK;
}

void solar_os_usb_ufi_msc_unbind(void *device)
{ if (cbi.device == device) memset(&cbi, 0, sizeof(cbi)); }
solar_os_usb_ufi_t *solar_os_usb_ufi_msc_state(void *device)
{ return device && cbi.device == device ? &cbi.ufi : NULL; }

esp_err_t solar_os_usb_ufi_msc_initialize(void *device, uint32_t *size, uint32_t *count)
{
    const solar_os_usb_ufi_io_t io = {.context = device, .command = adsc,
        .data = data_transfer, .status = status_transfer, .clear = clear, .delay = delay};
    esp_err_t err = solar_os_usb_ufi_init(&cbi.ufi, &io);
    *size = cbi.ufi.sector_size; *count = cbi.ufi.sector_count;
    return err;
}

esp_err_t solar_os_usb_ufi_msc_execute(void *device, const uint8_t *command, size_t length,
                                      bool input, void *data, size_t size)
{
    solar_os_usb_ufi_t *ufi = solar_os_usb_ufi_msc_state(device);
    return ufi ? solar_os_usb_ufi_execute(ufi, command, length, input, data, size) : ESP_ERR_INVALID_STATE;
}
