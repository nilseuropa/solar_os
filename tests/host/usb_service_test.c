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
static bool fail_pm, cdc_ready = true, cdc_busy, active, partial_start;
static esp_err_t start_error, stop_error, cdc_error;
static unsigned starts;
static usb_host_port_callback_t callback;

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
    assert(!active); /* Never restore CDC over an incompletely stopped host. */
    if (cdc_error != ESP_OK) return cdc_error;
    cdc_ready = true;
    return ESP_OK;
}
esp_err_t solar_os_cdc_deinit(void)
{
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
    if (stop_error != ESP_OK && active) return stop_error;
    active = false;
    return ESP_OK;
}
bool usb_host_port_is_active(void) { return active; }
size_t usb_host_port_interfaces(usb_host_port_interface_t *out, size_t capacity)
{ (void)out; (void)capacity; return 0; }

esp_err_t solar_os_usb_hid_input_open(solar_os_usb_hid_input_t *input, const char *name, uint8_t protocol)
{ (void)input; (void)name; (void)protocol; return ESP_OK; }
esp_err_t solar_os_usb_hid_input_report(solar_os_usb_hid_input_t *input, const uint8_t *data, size_t length)
{ (void)input; (void)data; (void)length; return ESP_OK; }
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

int main(void)
{
    status(SOLAR_OS_USB_MODE_DEVICE);
    assert(pm_locks == 0 && starts == 0);
    cdc_busy = true;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_ERR_INVALID_STATE);
    assert(cdc_ready && !active && pm_locks == 0 && solar_os_resource_claim_count() == 0);
    cdc_busy = false;

    assert(solar_os_resource_claim(SOLAR_OS_RESOURCE_GPIO_PIN, 19, -1, "other", "busy") == ESP_OK);
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_ERR_INVALID_STATE);
    assert(cdc_ready && starts == 0);
    solar_os_resource_release_owner("other");

    fail_pm = true;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_ERR_NO_MEM);
    assert(cdc_ready && pm_locks == 0 && solar_os_resource_claim_count() == 0);
    fail_pm = false;

    start_error = ESP_ERR_NO_MEM;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_ERR_NO_MEM);
    status(SOLAR_OS_USB_MODE_DEVICE);
    assert(cdc_ready && !active && pm_locks == 0 && solar_os_resource_claim_count() == 0);
    partial_start = true; stop_error = ESP_ERR_TIMEOUT;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_ERR_NO_MEM);
    assert(status(SOLAR_OS_USB_MODE_FAULT).rollback_error == ESP_ERR_TIMEOUT);
    assert(active && !cdc_ready && pm_locks == 2 && solar_os_resource_claim_count() == 3);
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_ERR_INVALID_STATE);
    stop_error = ESP_OK;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_DEVICE) == ESP_OK);
    assert(cdc_ready && pm_locks == 0);

    start_error = ESP_OK; partial_start = false;
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_OK);
    assert(active && !cdc_ready && pm_locks == 2 && callback);
    status(SOLAR_OS_USB_MODE_HOST);
    unsigned previous_starts = starts;
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
    assert(cdc_ready && pm_locks == 0 && solar_os_resource_claim_count() == 0);
    fail_pm = false;

    for (unsigned i = 0; i < 50; i++) {
        assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_OK);
        assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_DEVICE) == ESP_OK);
        assert(!active && cdc_ready && pm_locks == 0 && solar_os_resource_claim_count() == 0);
    }
    assert(solar_os_usb_prepare_sleep() == ESP_OK);
    assert(solar_os_usb_set_mode(SOLAR_OS_USB_MODE_HOST) == ESP_ERR_INVALID_STATE);
    assert(solar_os_usb_resume() == ESP_OK);
    status(SOLAR_OS_USB_MODE_DEVICE);
    puts("USB service lifecycle tests: ok");
    return 0;
}
