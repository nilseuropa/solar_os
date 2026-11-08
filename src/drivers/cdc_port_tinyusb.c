#include "cdc_port.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "class/hid/hid_device.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hid_port.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

#define USB_CDC_INTERFACE TINYUSB_CDC_ACM_0
#define USB_CDC_NOTIFICATION_EP 0x81
#define USB_CDC_DATA_OUT_EP 0x02
#define USB_CDC_DATA_IN_EP 0x82
#define USB_HID_IN_EP 0x83
#define USB_HID_POLL_INTERVAL_MS 5
#define USB_WRITE_TIMEOUT_MS 100

enum {
    USB_ITF_CDC,
    USB_ITF_CDC_DATA,
    USB_ITF_HID,
    USB_ITF_COUNT,
};

enum {
    USB_STRING_LANGID,
    USB_STRING_MANUFACTURER,
    USB_STRING_PRODUCT,
    USB_STRING_SERIAL,
    USB_STRING_CDC,
    USB_STRING_HID,
};

#define USB_CONFIG_TOTAL_LEN \
    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)

_Static_assert(sizeof(hid_port_keyboard_report_t) == sizeof(hid_keyboard_report_t),
               "keyboard report must match TinyUSB descriptor");
_Static_assert(sizeof(hid_port_mouse_report_t) == sizeof(hid_mouse_report_t),
               "mouse report must match TinyUSB descriptor");
_Static_assert(sizeof(hid_port_gamepad_report_t) == sizeof(hid_gamepad_report_t),
               "gamepad report must match TinyUSB descriptor");

static const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(HID_PORT_REPORT_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(HID_PORT_REPORT_MOUSE)),
    TUD_HID_REPORT_DESC_GAMEPAD(HID_REPORT_ID(HID_PORT_REPORT_GAMEPAD)),
};

static const uint8_t usb_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1,
                          USB_ITF_COUNT,
                          0,
                          USB_CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP,
                          100),
    TUD_CDC_DESCRIPTOR(USB_ITF_CDC,
                       USB_STRING_CDC,
                       USB_CDC_NOTIFICATION_EP,
                       8,
                       USB_CDC_DATA_OUT_EP,
                       USB_CDC_DATA_IN_EP,
                       64),
    TUD_HID_DESCRIPTOR(USB_ITF_HID,
                       USB_STRING_HID,
                       HID_ITF_PROTOCOL_NONE,
                       sizeof(hid_report_descriptor),
                       USB_HID_IN_EP,
                       CFG_TUD_HID_EP_BUFSIZE,
                       USB_HID_POLL_INTERVAL_MS),
};

static char usb_serial[13];
static const char *usb_string_descriptor[] = {
    (const char[]){0x09, 0x04},
    "SolarOS",
    "SolarOS composite device",
    usb_serial,
    "SolarOS cdc0",
    "SolarOS HID",
};

static SemaphoreHandle_t rx_signal;
static SemaphoreHandle_t tx_mutex;
static volatile bool mounted;
static bool ready;

static void usb_event_callback(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (event == NULL) {
        return;
    }
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        mounted = true;
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        mounted = false;
    }
}

static void cdc_rx_callback(int interface, cdcacm_event_t *event)
{
    (void)interface;
    (void)event;
    if (rx_signal != NULL) {
        xSemaphoreGive(rx_signal);
    }
}

static void make_usb_serial(void)
{
    uint8_t mac[6] = {0};
    if (esp_efuse_mac_get_default(mac) != ESP_OK) {
        snprintf(usb_serial, sizeof(usb_serial), "000000000000");
        return;
    }
    snprintf(usb_serial,
             sizeof(usb_serial),
             "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

esp_err_t cdc_port_init(const cdc_port_config_t *config)
{
    if (config == NULL || config->rx_buffer_size == 0 || config->tx_buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ready) {
        return ESP_OK;
    }

    rx_signal = xSemaphoreCreateBinary();
    tx_mutex = xSemaphoreCreateMutex();
    if (rx_signal == NULL || tx_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    make_usb_serial();
    tinyusb_config_t usb_config = TINYUSB_DEFAULT_CONFIG(usb_event_callback, NULL);
    usb_config.descriptor.full_speed_config = usb_configuration_descriptor;
    usb_config.descriptor.string = usb_string_descriptor;
    usb_config.descriptor.string_count =
        sizeof(usb_string_descriptor) / sizeof(usb_string_descriptor[0]);

    esp_err_t ret = tinyusb_driver_install(&usb_config);
    if (ret != ESP_OK) {
        return ret;
    }

    const tinyusb_config_cdcacm_t cdc_config = {
        .cdc_port = USB_CDC_INTERFACE,
        .callback_rx = cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };
    ret = tinyusb_cdcacm_init(&cdc_config);
    if (ret != ESP_OK) {
        return ret;
    }

    ready = true;
    (void)tinyusb_console_init(USB_CDC_INTERFACE);
    return ESP_OK;
}

bool cdc_port_is_ready(void)
{
    return ready;
}

esp_err_t cdc_port_deinit(void)
{
    /* Runtime role switching is only supported by the Serial/JTAG profile. */
    return ESP_ERR_NOT_SUPPORTED;
}

bool cdc_port_is_connected(void)
{
    return ready && mounted;
}

bool cdc_port_driver_installed(void)
{
    return ready;
}

esp_err_t cdc_port_write(const uint8_t *data, size_t len, size_t *written)
{
    if (written != NULL) {
        *written = 0;
    }
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }

    xSemaphoreTake(tx_mutex, portMAX_DELAY);
    size_t offset = 0;
    const TickType_t start = xTaskGetTickCount();
    while (offset < len) {
        offset += tinyusb_cdcacm_write_queue(USB_CDC_INTERFACE,
                                             data + offset,
                                             len - offset);
        esp_err_t ret = tinyusb_cdcacm_write_flush(USB_CDC_INTERFACE,
                                                   pdMS_TO_TICKS(USB_WRITE_TIMEOUT_MS));
        if (ret != ESP_OK && ret != ESP_ERR_NOT_FINISHED) {
            break;
        }
        if ((xTaskGetTickCount() - start) >= pdMS_TO_TICKS(USB_WRITE_TIMEOUT_MS)) {
            break;
        }
    }
    xSemaphoreGive(tx_mutex);

    if (written != NULL) {
        *written = offset;
    }
    return offset == len ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t cdc_port_read(uint8_t *data,
                        size_t len,
                        uint32_t timeout_ms,
                        size_t *read_len)
{
    if (read_len != NULL) {
        *read_len = 0;
    }
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }

    size_t received = 0;
    esp_err_t ret = tinyusb_cdcacm_read(USB_CDC_INTERFACE, data, len, &received);
    if (ret == ESP_OK && received == 0 && timeout_ms > 0 &&
        xSemaphoreTake(rx_signal, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        ret = tinyusb_cdcacm_read(USB_CDC_INTERFACE, data, len, &received);
    }
    if (read_len != NULL) {
        *read_len = received;
    }
    return ret;
}

bool hid_port_is_connected(void)
{
    return ready && mounted;
}

esp_err_t hid_port_send_report(uint8_t report_id, const void *report, size_t report_len)
{
    if (!hid_port_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (report == NULL || report_len == 0 || report_len > UINT16_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!tud_hid_ready()) {
        return ESP_ERR_NOT_FINISHED;
    }
    return tud_hid_report(report_id, report, (uint16_t)report_len)
        ? ESP_OK
        : ESP_FAIL;
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance,
                               uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer,
                               uint16_t requested_len)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)requested_len;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance,
                           uint8_t report_id,
                           hid_report_type_t report_type,
                           const uint8_t *buffer,
                           uint16_t buffer_size)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)buffer_size;
}
