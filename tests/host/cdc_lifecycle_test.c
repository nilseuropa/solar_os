#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cdc_port.h"
#include "solar_os_cdc.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_native_usb.h"
#include "solar_os_stream.h"
#include "solar_os_resources.h"

static bool installed, stream_registered, race_open, fail_unregister, fail_stop;
static bool fail_start, fail_register;
static bool fail_after_uninstall;
static solar_os_stream_driver_t stream_driver;

esp_err_t solar_os_stream_register(const solar_os_stream_driver_t *driver)
{ assert(!stream_registered); if (fail_register) return ESP_ERR_NO_MEM; stream_driver = *driver; stream_registered = true; return ESP_OK; }
esp_err_t solar_os_stream_register_many(const solar_os_stream_driver_t *drivers, size_t count)
{ assert(count == 1); return solar_os_stream_register(drivers); }
void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t cls, const char *tag)
{ (void)cls; (void)tag; return calloc(count, size); }
void solar_os_memory_free(void *data) { free(data); }
esp_err_t solar_os_stream_unregister(const char *name)
{
    assert(strcmp(name, "cdc0") == 0);
    if (race_open) {
        solar_os_port_handle_t handle = SOLAR_OS_PORT_HANDLE_INIT;
        assert(solar_os_port_claim(name, "racing-owner", &handle) == ESP_ERR_INVALID_STATE);
    }
    if (fail_unregister) return ESP_ERR_INVALID_STATE;
    stream_registered = false;
    return ESP_OK;
}
esp_err_t solar_os_stream_unregister_many(const char *const *names, size_t count)
{ assert(count == 1); return solar_os_stream_unregister(names[0]); }
esp_err_t solar_os_stream_unregister_many_owned(const char *const *names, const void *const *owners, size_t count)
{ assert(owners != NULL); return solar_os_stream_unregister_many(names, count); }

esp_err_t cdc_port_init(const cdc_port_config_t *config)
{ assert(config->rx_buffer_size && config->tx_buffer_size); if (fail_start) return ESP_ERR_NO_MEM; installed = true; return ESP_OK; }
esp_err_t cdc_port_deinit(void)
{ if (fail_stop) return ESP_FAIL; installed = false; return fail_after_uninstall ? ESP_FAIL : ESP_OK; }
bool cdc_port_is_ready(void) { return installed; }
bool cdc_port_is_connected(void) { return installed; }
bool cdc_port_driver_installed(void) { return installed; }
esp_err_t cdc_port_read(uint8_t *data, size_t length, uint32_t timeout, size_t *count)
{ (void)data; (void)length; (void)timeout; assert(installed); if (count) *count = 0; return ESP_OK; }
esp_err_t cdc_port_write(const uint8_t *data, size_t length, size_t *count)
{ (void)data; assert(installed); if (count) *count = length; return ESP_OK; }
esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *fmt, ...)
{ (void)level; (void)tag; (void)fmt; return ESP_OK; }
size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t length = strlen(src);
    if (size) { size_t n = length < size - 1 ? length : size - 1; memcpy(dst, src, n); dst[n] = 0; }
    return length;
}

int main(void)
{
    const solar_os_native_usb_cdc_api_v1_t *api = solar_os_native_usb_get_service(
        SOLAR_OS_NATIVE_USB_CDC_SERVICE, SOLAR_OS_NATIVE_USB_CDC_ABI, sizeof(*api));
    assert(api && api->struct_size == sizeof(*api));
    assert(!solar_os_native_usb_get_service(SOLAR_OS_NATIVE_USB_SERVICE, SOLAR_OS_NATIVE_USB_ABI, 8));
    assert(api->status(NULL) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(solar_os_port_init() == ESP_OK);
    assert(solar_os_cdc_deinit() == ESP_OK && !solar_os_resource_claim_count());
    for (int pin = 19; pin <= 20; pin++) {
        assert(solar_os_resource_claim(SOLAR_OS_RESOURCE_GPIO_PIN, pin, -1, "gpio", "GPIO") == ESP_OK);
        assert(solar_os_cdc_init() == ESP_ERR_INVALID_STATE && !installed);
        assert(solar_os_resource_claim_count() == 1);
        solar_os_resource_release_owner("gpio");
    }
    fail_start = true;
    assert(solar_os_cdc_init() == ESP_ERR_NO_MEM && !installed && !solar_os_resource_claim_count());
    fail_start = false; fail_register = true;
    assert(solar_os_cdc_init() == ESP_ERR_NO_MEM && !installed && !solar_os_resource_claim_count());
    fail_register = false;
    assert(solar_os_cdc_init() == ESP_OK && installed && stream_registered);
    assert(solar_os_resource_claim_count() == 3);
    assert(solar_os_resource_claim(SOLAR_OS_RESOURCE_GPIO_PIN, 19, -1, "gpio", "GPIO") == ESP_ERR_INVALID_STATE);
    solar_os_port_handle_t owner = SOLAR_OS_PORT_HANDLE_INIT;
    assert(solar_os_port_claim("cdc0", "job:log", &owner) == ESP_OK);
    assert(solar_os_cdc_deinit() == ESP_ERR_INVALID_STATE);
    solar_os_cdc_status_t status;
    assert(api->status(&status) == SOLAR_OS_NATIVE_OK);
    assert(status.initialized && status.port_claimed && !strcmp(status.port_owner, "job:log"));
    assert(solar_os_resource_claim_count() == 3);
    assert(solar_os_port_release(&owner) == ESP_OK);

    /* A new port claim cannot slip between stream and port unregister. */
    race_open = true;
    assert(solar_os_cdc_deinit() == ESP_OK && !installed && !stream_registered);
    assert(!api->status(&status) && !status.initialized && !status.port_claimed);
    assert(!solar_os_resource_claim_count());
    assert(solar_os_port_claim("cdc0", "shell", &owner) == ESP_ERR_NOT_FOUND);
    assert(solar_os_cdc_init() == ESP_OK);
    fail_unregister = true;
    assert(solar_os_cdc_deinit() == ESP_ERR_INVALID_STATE);
    assert(installed && stream_registered);
    assert(solar_os_port_claim("cdc0", "shell", &owner) == ESP_OK); /* Retirement rolled back. */
    assert(solar_os_port_release(&owner) == ESP_OK);
    fail_unregister = false;
    fail_stop = true;
    assert(solar_os_cdc_deinit() == ESP_FAIL);
    assert(installed && stream_registered);
    assert(solar_os_cdc_init() == ESP_ERR_INVALID_STATE);
    assert(solar_os_port_claim("cdc0", "shell", &owner) == ESP_OK);
    assert(solar_os_port_release(&owner) == ESP_OK);
    fail_stop = false;
    fail_after_uninstall = true;
    assert(solar_os_cdc_deinit() == ESP_FAIL && !installed);
    assert(solar_os_resource_claim_count() == 3);
    assert(solar_os_port_claim("cdc0", "shell", &owner) == ESP_ERR_INVALID_STATE);
    assert(solar_os_cdc_init() == ESP_ERR_INVALID_STATE);
    fail_after_uninstall = false;
    assert(solar_os_cdc_deinit() == ESP_OK && !solar_os_resource_claim_count());
    assert(solar_os_cdc_init() == ESP_OK);
    for (unsigned i = 0; i < 25; i++) {
        assert(solar_os_cdc_deinit() == ESP_OK && !installed && !stream_registered);
        assert(solar_os_cdc_init() == ESP_OK && installed && stream_registered);
    }
    assert(stream_driver.info.direction == SOLAR_OS_STREAM_DIRECTION_DUPLEX);
    puts("CDC ownership and lifecycle tests: ok");
    return 0;
}
