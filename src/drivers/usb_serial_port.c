#include "usb_serial_port.h"

#include <string.h>
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"

/* Generated address-aware entry points in the pinned VCP drivers. */
esp_err_t solar_os_usb_cp210x_open(const cdc_acm_host_open_config_t *, cdc_acm_dev_hdl_t *);
esp_err_t solar_os_usb_ch34x_open(const cdc_acm_host_open_config_t *, cdc_acm_dev_hdl_t *);

static usb_serial_port_discovery_fn discovery;

static void new_device(usb_device_handle_t handle)
{
    const usb_device_desc_t *device;
    const usb_config_desc_t *config;
    usb_device_info_t info;
    if (usb_host_get_device_descriptor(handle, &device) != ESP_OK ||
        usb_host_get_active_config_descriptor(handle, &config) != ESP_OK ||
        usb_host_device_info(handle, &info) != ESP_OK) return;
    const bool cp = device->idVendor == 0x10c4 &&
        (device->idProduct == 0xea60 || device->idProduct == 0xea70 || device->idProduct == 0xea71);
    const bool ch = device->idVendor == 0x1a86 &&
        (device->idProduct == 0x7522 || device->idProduct == 0x7523 || device->idProduct == 0x5523);
    const uint8_t *data = (const uint8_t *)config;
    for (size_t offset = config->bLength; offset + 2 <= config->wTotalLength;) {
        const unsigned length = data[offset];
        if (length < 2 || length > config->wTotalLength - offset) break;
        if (data[offset + 1] == USB_B_DESCRIPTOR_TYPE_INTERFACE && length >= sizeof(usb_intf_desc_t)) {
            const usb_intf_desc_t *iface = (const usb_intf_desc_t *)(data + offset);
            const bool acm = iface->bInterfaceClass == 2 && iface->bInterfaceSubClass == 2;
            if (iface->bAlternateSetting == 0 && (acm || ((cp || ch) && iface->bInterfaceClass == 0xff))) {
                const usb_serial_port_candidate_t candidate = {
                    .vid = device->idVendor, .pid = device->idProduct,
                    .address = info.dev_addr, .interface = iface->bInterfaceNumber,
                    .kind = cp ? SOLAR_OS_USB_SERIAL_CP210X : ch ? SOLAR_OS_USB_SERIAL_CH34X :
                        device->idVendor == 0x303a && device->idProduct == 0x1001 ?
                        SOLAR_OS_USB_SERIAL_JTAG : SOLAR_OS_USB_SERIAL_ACM,
                };
                discovery(&candidate);
            }
        }
        offset += length;
    }
}

static bool received(const uint8_t *data, size_t length, void *user)
{
    usb_serial_port_device_t *device = user;
    device->receive(data, length, device->user);
    return true;
}

static void event(const cdc_acm_host_dev_event_data_t *event, void *user)
{
    usb_serial_port_device_t *device = user;
    if (event->type == CDC_ACM_HOST_DEVICE_DISCONNECTED)
        device->event(ESP_OK, true, device->user);
    else if (event->type == CDC_ACM_HOST_ERROR)
        device->event(ESP_FAIL, false, device->user);
}

esp_err_t usb_serial_port_start(usb_serial_port_discovery_fn discovered)
{
    if (!discovered) return ESP_ERR_INVALID_ARG;
    discovery = discovered;
    const cdc_acm_host_driver_config_t config = {
        .driver_task_stack_size = 4096, .driver_task_priority = 4,
        .xCoreID = 0, .new_dev_cb = new_device,
    };
    return cdc_acm_host_install(&config);
}

esp_err_t usb_serial_port_stop(void)
{
    esp_err_t err = cdc_acm_host_uninstall();
    if (err == ESP_OK) discovery = NULL;
    return err;
}

esp_err_t usb_serial_port_open(const usb_serial_port_candidate_t *candidate,
    usb_serial_port_device_t *device, usb_serial_port_rx_fn receive,
    usb_serial_port_event_fn event_cb, void *user)
{
    if (!candidate || !device || !receive || !event_cb) return ESP_ERR_INVALID_ARG;
    *device = (usb_serial_port_device_t){.kind = candidate->kind, .user = user,
        .receive = receive, .event = event_cb};
    const cdc_acm_host_open_config_t config = {
        .vid = candidate->vid, .pid = candidate->pid, .dev_addr = candidate->address,
        .interface_idx = candidate->interface, .connection_timeout_ms = 0,
        .out_buffer_size = 512, .in_buffer_size = 512,
        .data_cb = received, .event_cb = event, .user_arg = device,
    };
    cdc_acm_dev_hdl_t handle = NULL;
    esp_err_t err = candidate->kind == SOLAR_OS_USB_SERIAL_CP210X ? solar_os_usb_cp210x_open(&config, &handle) :
        candidate->kind == SOLAR_OS_USB_SERIAL_CH34X ? solar_os_usb_ch34x_open(&config, &handle) :
        cdc_acm_host_open(&config, &handle);
    device->handle = handle;
    return err;
}

esp_err_t usb_serial_port_close(usb_serial_port_device_t *device)
{
    esp_err_t err = device->handle ? cdc_acm_host_close(device->handle) : ESP_OK;
    if (err == ESP_OK) device->handle = NULL;
    return err;
}

esp_err_t usb_serial_port_write(usb_serial_port_device_t *device, const uint8_t *data, size_t length)
{
    return length ? cdc_acm_host_data_tx_blocking(device->handle, data, length, 1000) : ESP_OK;
}

esp_err_t usb_serial_port_line(usb_serial_port_device_t *device, const solar_os_port_serial_config_t *config)
{
    if (device->kind == SOLAR_OS_USB_SERIAL_JTAG)
        return config->data_bits == 8 && !config->parity && !config->stop_bits ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
    const cdc_acm_line_coding_t line = {.dwDTERate = config->baud_rate,
        .bCharFormat = config->stop_bits, .bParityType = config->parity, .bDataBits = config->data_bits};
    return cdc_acm_host_line_coding_set(device->handle, &line);
}

esp_err_t usb_serial_port_lines(usb_serial_port_device_t *device, bool dtr, bool rts)
{
    return cdc_acm_host_set_control_line_state(device->handle, dtr, rts);
}

esp_err_t usb_serial_port_break(usb_serial_port_device_t *device, uint16_t duration_ms)
{
    return cdc_acm_host_send_break(device->handle, duration_ms);
}

const char *usb_serial_port_kind_name(usb_serial_port_kind_t kind)
{
    switch (kind) {
    case SOLAR_OS_USB_SERIAL_ACM: return "CDC-ACM";
    case SOLAR_OS_USB_SERIAL_JTAG: return "Serial/JTAG";
    case SOLAR_OS_USB_SERIAL_CP210X: return "CP210x";
    case SOLAR_OS_USB_SERIAL_CH34X: return "CH34x";
    }
    return "unknown";
}
