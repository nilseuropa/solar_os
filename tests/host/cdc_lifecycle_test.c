#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "cdc_port.h"
#include "solar_os_cdc.h"
#include "solar_os_log.h"
#include "solar_os_stream.h"

static bool installed, stream_registered, race_open, fail_unregister, fail_stop;
static solar_os_stream_driver_t stream_driver;

esp_err_t solar_os_stream_register(const solar_os_stream_driver_t *driver)
{ assert(!stream_registered); stream_driver = *driver; stream_registered = true; return ESP_OK; }
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
esp_err_t cdc_port_init(const cdc_port_config_t *config)
{ assert(config->rx_buffer_size && config->tx_buffer_size); installed = true; return ESP_OK; }
esp_err_t cdc_port_deinit(void)
{ if (fail_stop) return ESP_FAIL; installed = false; return ESP_OK; }
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
    assert(solar_os_port_init() == ESP_OK);
    assert(solar_os_cdc_init() == ESP_OK && installed && stream_registered);
    solar_os_port_handle_t owner = SOLAR_OS_PORT_HANDLE_INIT;
    assert(solar_os_port_claim("cdc0", "job:log", &owner) == ESP_OK);
    assert(solar_os_cdc_deinit() == ESP_ERR_INVALID_STATE);
    solar_os_cdc_status_t status;
    solar_os_cdc_get_status(&status);
    assert(status.initialized && status.port_claimed && !strcmp(status.port_owner, "job:log"));
    assert(solar_os_port_release(&owner) == ESP_OK);

    /* A new port claim cannot slip between stream and port unregister. */
    race_open = true;
    assert(solar_os_cdc_deinit() == ESP_OK && !installed && !stream_registered);
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
    assert(solar_os_port_claim("cdc0", "shell", &owner) == ESP_OK);
    assert(solar_os_port_release(&owner) == ESP_OK);
    fail_stop = false;
    for (unsigned i = 0; i < 25; i++) {
        assert(solar_os_cdc_deinit() == ESP_OK && !installed && !stream_registered);
        assert(solar_os_cdc_init() == ESP_OK && installed && stream_registered);
    }
    assert(stream_driver.info.direction == SOLAR_OS_STREAM_DIRECTION_DUPLEX);
    puts("CDC ownership and lifecycle tests: ok");
    return 0;
}
