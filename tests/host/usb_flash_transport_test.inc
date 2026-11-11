#include <assert.h>
#include <stddef.h>
#include <string.h>
#include "solar_os_port.h"
typedef struct { const void *ops; } esp_loader_port_t;
typedef enum { ESP_LOADER_SUCCESS, ESP_LOADER_ERROR_FAIL, ESP_LOADER_ERROR_TIMEOUT } esp_loader_error_t;
#define FLASH_OWNER "flash"
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#define pdMS_TO_TICKS(ms) (ms)
static int64_t clock_us;
static unsigned delays, gpio_calls, usb_reads, usb_writes, uart_reads, uart_writes, line_count;
static uint8_t lines[8];
static esp_err_t control_error, read_error;
static uint32_t rate;
static void vTaskDelay(unsigned ms) { delays += ms; clock_us += ms * 1000; }
static int64_t esp_timer_get_time(void) { clock_us += 1000; return clock_us; }
static esp_err_t solar_os_gpio_write(int pin, bool level) { (void)level; assert(pin >= 0); gpio_calls++; return ESP_OK; }
esp_err_t solar_os_port_serial_configure(const solar_os_port_handle_t *handle, const solar_os_port_serial_config_t *config, uint32_t mask) {
    assert(handle->index == 2);
    if (control_error) return control_error;
    if (mask == SOLAR_OS_SERIAL_BAUD) rate = config->baud_rate;
    else { assert(mask == (SOLAR_OS_SERIAL_DTR | SOLAR_OS_SERIAL_RTS)); assert(line_count < 8); lines[line_count++] = config->dtr * 2 + config->rts; }
    return ESP_OK;
}
static esp_err_t solar_os_uart_bus_set_baud_rate_owned(const char *name, uint32_t baud, const char *owner)
{ assert(!strcmp(name, "uart0") && !strcmp(owner, FLASH_OWNER)); rate = baud; return ESP_OK; }
esp_err_t solar_os_port_read(const solar_os_port_handle_t *handle, uint8_t *data, size_t size, uint32_t timeout, size_t *count)
{ assert(handle->index == 2 && size && timeout); usb_reads++; data[0] = 0xc0; *count = 1; return read_error; }
static esp_err_t solar_os_bus_uart_read(const char *name, uint8_t *data, size_t size, uint32_t timeout, size_t *count)
{ assert(!strcmp(name, "uart0") && size && timeout); uart_reads++; data[0] = 0xc0; *count = 1; return read_error; }
esp_err_t solar_os_port_write(const solar_os_port_handle_t *handle, const uint8_t *data, size_t size, size_t *written)
{ assert(handle->index == 2 && data && size); usb_writes++; *written = 1; return ESP_OK; }
static esp_err_t solar_os_bus_uart_write(const char *name, const uint8_t *data, size_t size, size_t *written)
{ assert(!strcmp(name, "uart0") && data && size); uart_writes++; *written = 1; return ESP_OK; }
