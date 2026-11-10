#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_pm.h"
#include "solar_os_usb.h"
#include "solar_os_cdc.h"
#include "solar_os_resources.h"
#include "solar_os_usb_hid_input.h"

struct usb_test_pm_lock { bool held; };
static unsigned pm_locks;
static bool fail_pm, cdc_ready = SOLAR_OS_BOARD_HAS_CDC, cdc_busy, active, partial_start;
static esp_err_t start_error, stop_error, cdc_error;
static unsigned starts;
static usb_host_port_callback_t callback;
static solar_os_usb_hid_input_t *opened_input;
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
    cdc_ready = true;
    return ESP_OK;
}
esp_err_t solar_os_cdc_deinit(void)
{
    assert(SOLAR_OS_BOARD_HAS_CDC);
    if (cdc_busy) return ESP_ERR_INVALID_STATE;
    cdc_ready = false;
    return ESP_OK;
}
esp_err_t usb_host_port_start(usb_host_port_callback_t cb)
{
    assert(!cdc_ready && !active && pm_locks == 2);
    starts++; callback = cb;
    active = start_error == ESP_OK || partial_start;
    return start_error;
}
esp_err_t usb_host_port_stop(void)
{
#if SOLAR_OS_PACKAGE_SERVICE_USB_STORAGE
    assert(!storage_active);
#endif
    if (stop_error != ESP_OK && active) return stop_error;
    active = false;
    return ESP_OK;
}
esp_err_t usb_host_port_enable(void)
{
    assert(active);
#if SOLAR_OS_PACKAGE_SERVICE_USB_STORAGE
    assert(storage_active);
#endif
    return ESP_OK;
}
bool usb_host_port_is_active(void) { return active; }
size_t usb_host_port_interfaces(usb_host_port_interface_t *out, size_t capacity)
{ (void)out; (void)capacity; return 0; }

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
    solar_os_usb_status_t result;
    solar_os_usb_get_status(&result);
    assert(result.mode == mode);
    return result;
}

static bool device_ready(void)
{
    return cdc_ready == (bool)SOLAR_OS_BOARD_HAS_CDC;
}

int main(void)
{
    status(SOLAR_OS_USB_MODE_DEVICE);
    assert(pm_locks == 0 && starts == 0);
#if SOLAR_OS_BOARD_HAS_CDC
    cdc_busy = true;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_ERR_INVALID_STATE);
    assert(cdc_ready && !active && pm_locks == 0 && solar_os_resource_claim_count() == 0);
    cdc_busy = false;
#endif

    for (int pin = 19; pin <= 20; pin++) {
        assert(solar_os_resource_claim(SOLAR_OS_RESOURCE_GPIO_PIN, pin, -1, "other", "busy") == ESP_OK);
        assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_ERR_INVALID_STATE);
        assert(device_ready() && starts == 0 && solar_os_resource_claim_count() == 1);
        solar_os_resource_release_owner("other");
    }

    fail_pm = true;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_ERR_NO_MEM);
    assert(device_ready() && pm_locks == 0 && solar_os_resource_claim_count() == 0);
    fail_pm = false;

    start_error = ESP_ERR_NO_MEM;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_ERR_NO_MEM);
    status(SOLAR_OS_USB_MODE_DEVICE);
    assert(device_ready() && !active && pm_locks == 0 && solar_os_resource_claim_count() == 0);
    partial_start = true; stop_error = ESP_ERR_TIMEOUT;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_ERR_NO_MEM);
    assert(status(SOLAR_OS_USB_MODE_FAULT).rollback_error == ESP_ERR_TIMEOUT);
    assert(active && !cdc_ready && pm_locks == 2 && solar_os_resource_claim_count() == 3);
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_ERR_INVALID_STATE);
    stop_error = ESP_OK;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_DEVICE) == ESP_OK);
    assert(device_ready() && pm_locks == 0);

    start_error = ESP_OK; partial_start = false;
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
    unsigned previous_starts = starts;
#if SOLAR_OS_PACKAGE_SERVICE_USB_STORAGE
    storage_stop_error = ESP_ERR_INVALID_STATE;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_DEVICE) == ESP_ERR_INVALID_STATE);
    assert(solar_os_usb_prepare_sleep() == ESP_ERR_INVALID_STATE);
    assert(active && storage_active && !cdc_ready && pm_locks == 2);
    assert(!status(SOLAR_OS_USB_MODE_HOST).sleep_prepared);
    storage_stop_error = ESP_OK;
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
    assert(!active && !cdc_ready && pm_locks == 0 && solar_os_resource_claim_count() == 3);
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_DEVICE) == ESP_ERR_INVALID_STATE);
    assert(solar_os_usb_resume() == ESP_OK);
    status(SOLAR_OS_USB_MODE_HOST);
    assert(active && !cdc_ready && pm_locks == 2);
    assert(solar_os_usb_prepare_sleep() == ESP_OK);
    fail_pm = true;
    assert(solar_os_usb_resume() == ESP_ERR_NO_MEM);
    status(SOLAR_OS_USB_MODE_DEVICE);
    assert(device_ready() && pm_locks == 0 && solar_os_resource_claim_count() == 0);
    fail_pm = false;

    for (unsigned i = 0; i < 50; i++) {
        assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_OK);
        assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_DEVICE) == ESP_OK);
        assert(!active && device_ready() && pm_locks == 0 && solar_os_resource_claim_count() == 0);
    }
#if SOLAR_OS_PACKAGE_SERVICE_USB_STORAGE
    storage_start_error = ESP_ERR_NO_MEM;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_ERR_NO_MEM);
    status(SOLAR_OS_USB_MODE_DEVICE);
    assert(!active && !storage_active && device_ready() && pm_locks == 0);
    storage_start_error = ESP_OK;
#endif
    assert(solar_os_usb_prepare_sleep() == ESP_OK);
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_ERR_INVALID_STATE);
    assert(solar_os_usb_resume() == ESP_OK);
    status(SOLAR_OS_USB_MODE_DEVICE);
    puts("USB service lifecycle tests: ok");
    return 0;
}
