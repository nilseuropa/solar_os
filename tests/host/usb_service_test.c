#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_pm.h"
#include "solar_os_usb.h"
#include "solar_os_cdc.h"
#include "solar_os_resources.h"
#include "solar_os_usb_hid_input.h"
#include "solar_os_native_usb.h"
#include "solar_os_native_usb_serial_abi.h"
#include "driver/gpio.h"
#include "solar_os_board_caps.h"
#if SOLAR_OS_PACKAGE_SERVICE_USB_SERIAL
#include "solar_os_usb_serial.h"
#endif

struct usb_test_pm_lock { bool held; };
static unsigned pm_locks;
static bool fail_pm, cdc_ready, cdc_busy, active, partial_start;
static bool pads_enabled = true, fail_pin_reset;
static unsigned pin_resets;
#define USB_DEVICE_CLAIM_COUNT (SOLAR_OS_BOARD_HAS_CDC ? 3U : 0U)
static esp_err_t start_error, stop_error, cdc_error;
static esp_err_t enable_error;
static unsigned starts;
static usb_host_port_callback_t callback;
static solar_os_usb_hid_input_t *opened_input;

esp_err_t gpio_config(const gpio_config_t *config)
{
    assert(config->pin_bit_mask == ((1ULL << 19) | (1ULL << 20)));
    assert(config->mode == GPIO_MODE_DISABLE && !config->pull_up_en && !config->pull_down_en && !config->intr_type);
    solar_os_resource_claim_t claim;
    assert(solar_os_resource_find_claim(SOLAR_OS_RESOURCE_GPIO_PIN, 19, -1, &claim));
    assert(!strcmp(claim.owner, "usb-host"));
    assert(!active && !cdc_ready);
    if (fail_pin_reset) return ESP_FAIL;
    pads_enabled = false;
    pin_resets++;
    return ESP_OK;
}
#ifdef SOLAR_OS_BOARD_PIN_USB_VBUS_EN
#include "driver/gpio.h"
#define USB_HOST_CLAIM_COUNT 4U
static bool vbus_power, fail_vbus_enable, fail_vbus_disable;
static unsigned power_ons;

esp_err_t gpio_hold_dis(gpio_num_t pin)
{ assert(pin == 18); return ESP_OK; }
esp_err_t gpio_sleep_sel_dis(gpio_num_t pin)
{ assert(pin == 18); return ESP_OK; }
esp_err_t gpio_set_level(gpio_num_t pin, unsigned level)
{
    assert(pin == 18); /* VBUS must never toggle the shared GPIO21 boost rail. */
    if ((level && fail_vbus_enable) || (!level && fail_vbus_disable)) return ESP_FAIL;
    if (level) power_ons++;
    vbus_power = level != 0;
    return ESP_OK;
}
esp_err_t gpio_set_direction(gpio_num_t pin, int mode)
{ assert(pin == 18 && mode == GPIO_MODE_OUTPUT); return ESP_OK; }
#else
#define USB_HOST_CLAIM_COUNT 3U
#endif

static bool vbus_ready(bool enabled)
{
#ifdef SOLAR_OS_BOARD_PIN_USB_VBUS_EN
    return vbus_power == enabled;
#else
    (void)enabled;
    return true;
#endif
}
#if SOLAR_OS_PACKAGE_SERVICE_USB_STORAGE
static bool storage_active;
static esp_err_t storage_start_error, storage_stop_error;
esp_err_t solar_os_usb_storage_start(void)
{ assert(active); storage_active = storage_start_error == ESP_OK; return storage_start_error; }
esp_err_t solar_os_usb_storage_stop(void)
{
    if (storage_active && storage_stop_error != ESP_OK) return storage_stop_error;
    storage_active = false;
    return ESP_OK;
}
void solar_os_usb_storage_get_status(solar_os_usb_storage_status_t *status)
{
    *status = (solar_os_usb_storage_status_t){.connected = storage_active, .address = 4,
        .sector_size = 512, .sector_count = 2880, .last_error = storage_stop_error};
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_USB_SERIAL
static bool serial_active, serial_accepting;
static esp_err_t serial_start_error, serial_prepare_error;
esp_err_t solar_os_usb_serial_start(void)
{ assert(active && vbus_ready(false)); serial_active = serial_start_error == ESP_OK; serial_accepting = serial_active; return serial_start_error; }
esp_err_t solar_os_usb_serial_prepare_stop(void)
{ if (serial_active && serial_prepare_error != ESP_OK) return serial_prepare_error; serial_accepting = false; return ESP_OK; }
void solar_os_usb_serial_cancel_stop(void) { serial_accepting = serial_active; }
esp_err_t solar_os_usb_serial_stop(void)
{ serial_active = serial_accepting = false; return ESP_OK; }
size_t solar_os_usb_serial_list(solar_os_usb_serial_info_t *out, size_t capacity)
{
    if (!serial_active) return 0;
    if (out && capacity) out[0] = (solar_os_usb_serial_info_t){
        .port = "usbser0", .device = {.vid = 0x10c4, .pid = 0xea60, .address = 4,
            .interface = 1, .kind = SOLAR_OS_USB_SERIAL_CP210X},
        .config = {.baud_rate = 115200, .data_bits = 8, .dtr = true}, .connected = true,
        .received = 42, .transmitted = 10, .dropped = 3, .last_error = ESP_ERR_TIMEOUT,
    };
    if (out && capacity > 1) out[1] = (solar_os_usb_serial_info_t){.port = "usbser1"};
    return 2;
}
esp_err_t solar_os_usb_serial_last_error(void) { return ESP_ERR_NO_MEM; }
#endif

esp_err_t esp_pm_lock_create(esp_pm_lock_type_t type, int arg, const char *name,
                              esp_pm_lock_handle_t *handle)
{
    (void)type; (void)arg; (void)name;
    if (fail_pm) return ESP_ERR_NO_MEM;
    *handle = calloc(1, sizeof(**handle)); assert(*handle);
    pm_locks++;
    return ESP_OK;
}
esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t handle) { assert(!handle->held); handle->held = true; return ESP_OK; }
esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t handle) { assert(handle->held); handle->held = false; return ESP_OK; }
esp_err_t esp_pm_lock_delete(esp_pm_lock_handle_t handle) { assert(!handle->held); free(handle); pm_locks--; return ESP_OK; }

esp_err_t solar_os_cdc_init(void)
{
    assert(SOLAR_OS_BOARD_HAS_CDC); /* UART-only boards must never touch CDC. */
    assert(!active); /* Never restore CDC over an incompletely stopped host. */
    if (cdc_error != ESP_OK) return cdc_error;
    const solar_os_resource_request_t pins[] = {
        {SOLAR_OS_RESOURCE_USB_PORT, 0, -1, "USB"},
        {SOLAR_OS_RESOURCE_GPIO_PIN, 19, -1, "D-"},
        {SOLAR_OS_RESOURCE_GPIO_PIN, 20, -1, "D+"},
    };
    esp_err_t ret = solar_os_resource_claim_bundle(pins, 3, "usb-device", NULL);
    if (ret != ESP_OK) return ret;
    pads_enabled = true;
    cdc_ready = true;
    return ESP_OK;
}
esp_err_t solar_os_cdc_deinit(void)
{
    assert(SOLAR_OS_BOARD_HAS_CDC);
    if (cdc_busy) return ESP_ERR_INVALID_STATE;
    cdc_ready = false;
    pads_enabled = false;
    solar_os_resource_release_owner("usb-device");
    return ESP_OK;
}
void solar_os_cdc_get_status(solar_os_cdc_status_t *status)
{ *status = (solar_os_cdc_status_t){.initialized = cdc_ready, .driver_installed = cdc_ready}; }
esp_err_t usb_host_port_start(usb_host_port_callback_t cb)
{
    assert(!cdc_ready && !active && pm_locks == 2 && vbus_ready(false));
    starts++; callback = cb;
    active = start_error == ESP_OK || partial_start;
    if (active) pads_enabled = true;
    return start_error;
}
esp_err_t usb_host_port_stop(void)
{
#if SOLAR_OS_PACKAGE_SERVICE_USB_SERIAL
    assert(!serial_active);
#endif
#if SOLAR_OS_PACKAGE_SERVICE_USB_STORAGE
    assert(!storage_active);
#endif
    if (stop_error != ESP_OK && active) return stop_error;
    active = false;
    return ESP_OK;
}
esp_err_t usb_host_port_enable(void)
{
    assert(active && vbus_ready(true));
#if SOLAR_OS_PACKAGE_SERVICE_USB_STORAGE
    assert(storage_active);
#endif
#if SOLAR_OS_PACKAGE_SERVICE_USB_SERIAL
    assert(serial_active && serial_accepting);
#endif
    return enable_error;
}
bool usb_host_port_is_active(void) { return active; }
size_t usb_host_port_interfaces(solar_os_usb_interface_status_t *out, size_t capacity)
{
    if (!active || !capacity) return 0;
    out[0] = (solar_os_usb_interface_status_t){.address = 3, .vendor_id = 0x1234,
        .protocol = SOLAR_OS_NATIVE_USB_HID_MOUSE, .ready = true, .reports = 42,
        .protocol_error = ESP_ERR_NOT_SUPPORTED, .last_error = ESP_ERR_TIMEOUT};
    return 1;
}

esp_err_t solar_os_usb_hid_input_open(solar_os_usb_hid_input_t *input, const char *name, uint8_t protocol)
{ (void)name; memset(input, 0, sizeof(*input)); input->protocol = protocol; opened_input = input; return ESP_OK; }
esp_err_t solar_os_usb_hid_input_report(solar_os_usb_hid_input_t *input, const uint8_t *data, size_t length)
{
    assert(input == opened_input && input->mouse_layout.valid);
    assert(input->mouse_layout.report_id == 1 && input->mouse_layout.report_bytes == 6);
    assert(data[0] == 1 && length == 6);
    return ESP_OK;
}
void solar_os_usb_hid_input_reset(solar_os_usb_hid_input_t *input) { (void)input; }
void solar_os_usb_hid_input_close(solar_os_usb_hid_input_t *input) { (void)input; }

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t length = strlen(src);
    if (size) { size_t n = length < size - 1 ? length : size - 1; memcpy(dst, src, n); dst[n] = 0; }
    return length;
}

static solar_os_usb_status_t status(solar_os_usb_mode_t mode)
{
    if (mode == SOLAR_OS_USB_MODE_DEVICE && !SOLAR_OS_BOARD_HAS_CDC) mode = SOLAR_OS_USB_MODE_OFF;
    solar_os_usb_status_t result;
    solar_os_usb_get_status(&result);
    assert(result.mode == mode);
#ifdef SOLAR_OS_BOARD_PIN_USB_VBUS_EN
    assert(result.vbus_enabled == vbus_power);
#else
    assert(!result.vbus_enabled);
#endif
    return result;
}

static bool device_ready(void)
{
    return cdc_ready == (bool)SOLAR_OS_BOARD_HAS_CDC && vbus_ready(false);
}

int main(void)
{
    const solar_os_native_usb_api_v1_t *api = solar_os_native_usb_get_service(
        SOLAR_OS_NATIVE_USB_SERVICE, SOLAR_OS_NATIVE_USB_ABI, sizeof(*api));
    assert(api && api->set_mode(SOLAR_OS_USB_MODE_SUSPENDED) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!strcmp(api->mode_name(SOLAR_OS_USB_MODE_HOST), "host"));
#if SOLAR_OS_BOARD_HAS_CDC && !SOLAR_OS_BOARD_USB_DEFAULT_OFF
    cdc_error = ESP_ERR_NO_MEM;
    assert(solar_os_usb_init() == ESP_ERR_NO_MEM);
    status(SOLAR_OS_USB_MODE_FAULT);
    assert(solar_os_usb_init() == ESP_ERR_INVALID_STATE);
    assert(pads_enabled && !solar_os_resource_claim_count());
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_OFF) == ESP_OK);
    assert(!pads_enabled && !solar_os_resource_claim_count());
    cdc_error = ESP_OK;
#else
    assert(solar_os_resource_claim(SOLAR_OS_RESOURCE_GPIO_PIN, 19, -1, "other", "busy") == ESP_OK);
    assert(solar_os_usb_init() == ESP_ERR_INVALID_STATE);
    status(SOLAR_OS_USB_MODE_FAULT);
    assert(pads_enabled && solar_os_resource_claim_count() == 1);
    solar_os_resource_release_owner("other");
#endif
    assert(solar_os_usb_init() == ESP_OK);
#if SOLAR_OS_BOARD_USB_DEFAULT_OFF
    status(SOLAR_OS_USB_MODE_OFF);
    assert(!pads_enabled && !cdc_ready && solar_os_resource_claim_count() == 0);
#endif
    assert(api->set_mode(SOLAR_OS_USB_MODE_DEVICE) == SOLAR_OS_NATIVE_OK);
    status(SOLAR_OS_USB_MODE_DEVICE);
    solar_os_cdc_status_t cdc;
#if SOLAR_OS_BOARD_HAS_CDC
    assert((api->capabilities & SOLAR_OS_NATIVE_USB_CAP_CDC) && !api->cdc_status(&cdc) && cdc.driver_installed);
#else
    assert(!(api->capabilities & SOLAR_OS_NATIVE_USB_CAP_CDC));
    assert(api->cdc_status(&cdc) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED && !cdc.driver_installed);
#endif
    assert(pm_locks == 0 && starts == 0);
#if SOLAR_OS_BOARD_HAS_CDC
    cdc_busy = true;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_OFF) == ESP_ERR_INVALID_STATE);
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_ERR_INVALID_STATE);
    assert(cdc_ready && !active && pm_locks == 0 && solar_os_resource_claim_count() == USB_DEVICE_CLAIM_COUNT);
    cdc_busy = false;
#endif

    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_OFF) == ESP_OK);
    for (int pin = 19; pin <= 20; pin++) {
        assert(solar_os_resource_claim(SOLAR_OS_RESOURCE_GPIO_PIN, pin, -1, "other", "busy") == ESP_OK);
        unsigned resets = pin_resets;
        assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_OFF) == ESP_OK);
        assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_ERR_INVALID_STATE);
        if (SOLAR_OS_BOARD_HAS_CDC) assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_DEVICE) == ESP_ERR_INVALID_STATE);
        assert(!cdc_ready && !pads_enabled && pin_resets == resets && starts == 0 && solar_os_resource_claim_count() == 1);
        status(SOLAR_OS_USB_MODE_OFF);
        solar_os_resource_release_owner("other");
    }
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_DEVICE) == ESP_OK);
#ifdef SOLAR_OS_BOARD_PIN_USB_VBUS_EN
    assert(solar_os_resource_claim(SOLAR_OS_RESOURCE_GPIO_PIN, 18, -1, "other", "busy") == ESP_OK);
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_ERR_INVALID_STATE);
    assert(device_ready() && starts == 0 && solar_os_resource_claim_count() == USB_DEVICE_CLAIM_COUNT + 1);
    solar_os_resource_release_owner("other");
#endif

    fail_pm = true;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_ERR_NO_MEM);
    assert(device_ready() && pm_locks == 0 && solar_os_resource_claim_count() == USB_DEVICE_CLAIM_COUNT);
    fail_pm = false;

    start_error = ESP_ERR_NO_MEM;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_ERR_NO_MEM);
    status(SOLAR_OS_USB_MODE_DEVICE);
    assert(device_ready() && !active && pm_locks == 0 && solar_os_resource_claim_count() == USB_DEVICE_CLAIM_COUNT);
    partial_start = true; stop_error = ESP_ERR_TIMEOUT;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_ERR_NO_MEM);
    assert(status(SOLAR_OS_USB_MODE_FAULT).rollback_error == ESP_ERR_TIMEOUT);
    assert(active && !cdc_ready && pm_locks == 2 && solar_os_resource_claim_count() == USB_HOST_CLAIM_COUNT);
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_ERR_INVALID_STATE);
    stop_error = ESP_OK;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_DEVICE) == ESP_OK);
    assert(device_ready() && pm_locks == 0);

    start_error = ESP_OK; partial_start = false;
#ifdef SOLAR_OS_BOARD_PIN_USB_VBUS_EN
    fail_vbus_enable = true;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_FAIL);
    status(SOLAR_OS_USB_MODE_DEVICE);
    assert(!active && device_ready() && pm_locks == 0 && solar_os_resource_claim_count() == USB_DEVICE_CLAIM_COUNT);
    fail_vbus_enable = false;

    unsigned previous_power_ons = power_ons;
    enable_error = ESP_FAIL;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_FAIL);
    status(SOLAR_OS_USB_MODE_DEVICE);
    assert(power_ons == previous_power_ons + 1); /* Rollback after VBUS was enabled. */
    assert(!active && device_ready() && pm_locks == 0 && solar_os_resource_claim_count() == USB_DEVICE_CLAIM_COUNT);
    enable_error = ESP_OK;

    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_OK);
    fail_vbus_disable = true;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_DEVICE) == ESP_FAIL);
    status(SOLAR_OS_USB_MODE_FAULT);
    assert(!active && vbus_power && pm_locks == 2);
    assert(solar_os_resource_claim_count() == USB_HOST_CLAIM_COUNT);
    fail_vbus_disable = false;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_DEVICE) == ESP_OK);
    assert(device_ready() && pm_locks == 0 && solar_os_resource_claim_count() == USB_DEVICE_CLAIM_COUNT);
#endif
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_OK);
    assert(active && !cdc_ready && pm_locks == 2 && callback);
    for (int pin = 19; pin <= 20; pin++) {
        assert(solar_os_resource_claim(SOLAR_OS_RESOURCE_GPIO_PIN, pin, -1, "other", "busy") == ESP_ERR_INVALID_STATE);
    }
    usb_host_port_interface_t mouse = {
        .slot = 1, .protocol = SOLAR_OS_USB_HID_MOUSE,
        .mouse_layout = {.valid = true, .numbered = true, .report_id = 1, .report_bytes = 6},
    };
    assert(callback(USB_HOST_PORT_OPEN, &mouse, NULL, 0) == ESP_OK);
    const uint8_t report[] = {1, 0, 0xed, 0xff, 0, 0};
    assert(callback(USB_HOST_PORT_REPORT, &mouse, report, sizeof(report)) == ESP_OK);
    assert(callback(USB_HOST_PORT_CLOSE, &mouse, NULL, 0) == ESP_OK);
    solar_os_usb_hid_joystick_layout_t joystick_layout = {.valid = true, .numbered = true, .report_count = 1};
    joystick_layout.reports[0].id = 7;
    usb_host_port_interface_t joystick = {
        .slot = 2, .protocol = SOLAR_OS_USB_HID_JOYSTICK, .joystick_layout = &joystick_layout,
    };
    assert(callback(USB_HOST_PORT_OPEN, &joystick, NULL, 0) == ESP_OK);
    assert(opened_input->protocol == SOLAR_OS_USB_HID_JOYSTICK);
    assert(opened_input->joystick_layout.valid && opened_input->joystick_layout.reports[0].id == 7);
    assert(&opened_input->joystick_layout != &joystick_layout); /* Driver configuration is copied, not borrowed. */
    assert(callback(USB_HOST_PORT_CLOSE, &joystick, NULL, 0) == ESP_OK);
    memset(&mouse.mouse_layout, 0, sizeof(mouse.mouse_layout));
    assert(callback(USB_HOST_PORT_OPEN, &mouse, NULL, 0) == ESP_OK);
    assert(!opened_input->mouse_layout.valid); /* Boot fallback cannot retain the previous report layout. */
    assert(callback(USB_HOST_PORT_CLOSE, &mouse, NULL, 0) == ESP_OK);
    status(SOLAR_OS_USB_MODE_HOST);
#if SOLAR_OS_PACKAGE_SERVICE_USB_SERIAL
    serial_prepare_error = ESP_ERR_INVALID_STATE;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_DEVICE) == ESP_ERR_INVALID_STATE);
    assert(solar_os_usb_prepare_sleep() == ESP_ERR_INVALID_STATE);
    assert(active && serial_active && serial_accepting && storage_active && pm_locks == 2);
    serial_prepare_error = ESP_OK;
#endif
    unsigned previous_starts = starts;
#if SOLAR_OS_PACKAGE_SERVICE_USB_STORAGE
    storage_stop_error = ESP_ERR_INVALID_STATE;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_DEVICE) == ESP_ERR_INVALID_STATE);
    assert(solar_os_usb_prepare_sleep() == ESP_ERR_INVALID_STATE);
    assert(active && storage_active && !cdc_ready && pm_locks == 2);
    assert(vbus_ready(true)); /* Busy storage must keep its supply. */
    assert(!status(SOLAR_OS_USB_MODE_HOST).sleep_prepared);
    storage_stop_error = ESP_OK;
#if SOLAR_OS_PACKAGE_SERVICE_USB_SERIAL
    assert(serial_active && serial_accepting); /* Busy disk rolls back serial claim gating. */
#endif
#endif
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_OK && starts == previous_starts);
    stop_error = ESP_ERR_TIMEOUT;
    assert(solar_os_usb_prepare_sleep() == ESP_ERR_TIMEOUT);
    assert(active && pm_locks == 2);
    status(SOLAR_OS_USB_MODE_FAULT);
    stop_error = ESP_OK;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_DEVICE) == ESP_OK);
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_OK);
    assert(solar_os_usb_prepare_sleep() == ESP_OK);
    assert(status(SOLAR_OS_USB_MODE_SUSPENDED).sleep_prepared);
    assert(!active && !cdc_ready && pm_locks == 0 && solar_os_resource_claim_count() == USB_HOST_CLAIM_COUNT);
    assert(vbus_ready(false));
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_DEVICE) == ESP_ERR_INVALID_STATE);
    assert(solar_os_usb_resume() == ESP_OK);
    status(SOLAR_OS_USB_MODE_HOST);
    assert(active && !cdc_ready && pm_locks == 2);
    assert(solar_os_usb_prepare_sleep() == ESP_OK);
    fail_pm = true;
    assert(solar_os_usb_resume() == ESP_ERR_NO_MEM);
    status(SOLAR_OS_USB_MODE_DEVICE);
    assert(device_ready() && pm_locks == 0 && solar_os_resource_claim_count() == USB_DEVICE_CLAIM_COUNT);
    fail_pm = false;

    for (unsigned i = 0; i < 50; i++) {
        assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_OK);
        assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_DEVICE) == ESP_OK);
        assert(!active && device_ready() && pm_locks == 0 && solar_os_resource_claim_count() == USB_DEVICE_CLAIM_COUNT);
    }
#if SOLAR_OS_PACKAGE_SERVICE_USB_STORAGE
    storage_start_error = ESP_ERR_NO_MEM;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_ERR_NO_MEM);
    status(SOLAR_OS_USB_MODE_DEVICE);
    assert(!active && !storage_active && device_ready() && pm_locks == 0);
    storage_start_error = ESP_OK;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_USB_SERIAL
    serial_start_error = ESP_ERR_NO_MEM;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_ERR_NO_MEM);
    status(SOLAR_OS_USB_MODE_DEVICE);
    assert(!active && !serial_active && !storage_active && device_ready() && pm_locks == 0);
    serial_start_error = ESP_OK;
#endif
    assert(solar_os_usb_prepare_sleep() == ESP_OK);
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_ERR_INVALID_STATE);
    assert(solar_os_usb_resume() == ESP_OK);
    status(SOLAR_OS_USB_MODE_DEVICE);
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_OFF) == ESP_OK);
    unsigned resets = pin_resets;
    assert(solar_os_usb_prepare_sleep() == ESP_OK);
    assert(status(SOLAR_OS_USB_MODE_OFF).sleep_prepared);
    assert(solar_os_usb_resume() == ESP_OK && pin_resets == resets);
    assert(!pads_enabled && !pm_locks && solar_os_resource_claim_count() == 0);
    start_error = ESP_ERR_NO_MEM;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_ERR_NO_MEM);
    status(SOLAR_OS_USB_MODE_OFF);
    assert(!pads_enabled && !cdc_ready && !pm_locks && !solar_os_resource_claim_count());
    start_error = ESP_OK;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_OK);
    fail_pin_reset = true;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_OFF) == ESP_FAIL);
    status(SOLAR_OS_USB_MODE_FAULT);
    assert(!active && pm_locks == 2 && solar_os_resource_claim_count() == USB_HOST_CLAIM_COUNT);
    fail_pin_reset = false;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_OFF) == ESP_OK);
    assert(!pads_enabled && !pm_locks && !solar_os_resource_claim_count());
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_OK);
    assert(solar_os_usb_prepare_sleep() == ESP_OK);
    fail_pm = true;
    assert(solar_os_usb_resume() == ESP_ERR_NO_MEM);
    status(SOLAR_OS_USB_MODE_OFF);
    assert(!pads_enabled && !cdc_ready && !pm_locks && !solar_os_resource_claim_count());
    fail_pm = false;
#if SOLAR_OS_BOARD_HAS_CDC
    cdc_error = ESP_ERR_NO_MEM;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_DEVICE) == ESP_ERR_NO_MEM);
    status(SOLAR_OS_USB_MODE_OFF);
    assert(!pads_enabled && !solar_os_resource_claim_count());
    cdc_error = ESP_OK;
#endif
    assert(solar_os_usb_init() == ESP_OK); /* Repeated init never changes a user-selected mode. */
    status(SOLAR_OS_USB_MODE_OFF);
    assert(!api->set_mode(SOLAR_OS_USB_MODE_HOST));
    solar_os_usb_status_t snapshot;
    assert(!api->status(&snapshot) && snapshot.mode == SOLAR_OS_USB_MODE_HOST);
    assert(snapshot.interface_count == 1 && snapshot.interfaces[0].address == 3);
    assert(snapshot.interfaces[0].ready && snapshot.interfaces[0].reports == 42);
    assert(snapshot.interfaces[0].last_error == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    assert(snapshot.interfaces[0].protocol_error == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    solar_os_usb_storage_status_t storage;
#if SOLAR_OS_PACKAGE_SERVICE_USB_STORAGE
    assert((api->capabilities & SOLAR_OS_NATIVE_USB_CAP_STORAGE) && !api->storage_status(&storage));
    assert(storage.connected && storage.sector_count == 2880);
#else
    assert(!(api->capabilities & SOLAR_OS_NATIVE_USB_CAP_STORAGE));
    assert(api->storage_status(&storage) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED && !storage.connected);
#endif
    const solar_os_native_usb_serial_api_v1_t *serial_api = solar_os_native_usb_get_service(
        SOLAR_OS_NATIVE_USB_SERIAL_SERVICE, SOLAR_OS_NATIVE_USB_SERIAL_ABI, sizeof(*serial_api));
#if SOLAR_OS_PACKAGE_SERVICE_USB_SERIAL
    assert(serial_api && (api->capabilities & SOLAR_OS_NATIVE_USB_CAP_SERIAL));
    size_t serial_total = 99;
    int serial_error = 99;
    solar_os_native_usb_serial_info_v1_t serial;
    assert(serial_api->snapshot(NULL, 1, &serial_total, &serial_error) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!serial_total && !serial_error);
    assert(!serial_api->snapshot(NULL, 0, &serial_total, &serial_error) && serial_total == 2);
    assert(serial_error == SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
    assert(!serial_api->snapshot(&serial, 1, &serial_total, NULL) && serial_total == 2);
    assert(!strcmp(serial.port, "usbser0") && serial.kind == SOLAR_OS_USB_SERIAL_CP210X);
    assert(serial.address == 4 && serial.interface == 1 && serial.vendor_id == 0x10c4);
    assert(serial.config.baud_rate == 115200 && serial.config.data_bits == 8 && serial.config.dtr);
    assert(serial.received == 42 && serial.transmitted == 10 && serial.dropped == 3);
    assert(serial.last_error == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
#else
    assert(!serial_api && !(api->capabilities & SOLAR_OS_NATIVE_USB_CAP_SERIAL));
#endif
    assert(!api->set_mode(SOLAR_OS_USB_MODE_OFF));
    assert(!api->status(&snapshot) && !snapshot.interface_count);
#if SOLAR_OS_PACKAGE_SERVICE_USB_SERIAL
    assert(!serial_api->snapshot(NULL, 0, &serial_total, NULL) && !serial_total);
    assert(!strcmp(serial.port, "usbser0") && serial.connected);
#endif
    puts("USB service lifecycle and copied ABI status: ok");
    return 0;
}
