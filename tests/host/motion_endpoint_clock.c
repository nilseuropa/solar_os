#include <stdint.h>
#include "esp_err.h"
#include "freertos/task.h"
__attribute__((weak)) void vTaskDelay(TickType_t ticks) { (void)ticks; }
__attribute__((weak)) uint64_t solar_os_time_uptime_ms(void) { return 0; }
__attribute__((weak)) int64_t esp_timer_get_time(void) { return 0; }
__attribute__((weak)) esp_err_t solar_os_time_get_utc_epoch_ms(uint64_t *time)
{ (void)time;return ESP_ERR_NOT_SUPPORTED; }
