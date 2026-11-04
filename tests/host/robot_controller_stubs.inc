/* Unused generic controller endpoints. Robot tests exercise the real OS
 * controller registry/PID and the real motor ABI (integration test). */
#include "solar_os_controllers.h"
#include "solar_os_native_controllers.h"
#include "solar_os_pwm.h"
#include "esp_timer.h"
uint64_t solar_os_time_uptime_ms(void) { return (uint64_t)esp_timer_get_time()/1000; }
esp_err_t solar_os_stream_open(const char *id, const char *owner, solar_os_stream_handle_t *h)
{ (void)id;(void)owner;(void)h;return ESP_ERR_NOT_SUPPORTED; }
esp_err_t solar_os_stream_close_ex(solar_os_stream_handle_t *h)
{ (void)h;return ESP_OK; }
esp_err_t solar_os_stream_read_scalar_sample(solar_os_stream_handle_t *h,
    const solar_os_stream_read_options_t *o, solar_os_stream_scalar_sample_t *s)
{ (void)h;(void)o;(void)s;return ESP_ERR_NOT_SUPPORTED; }
bool solar_os_pwm_parse_output(const char *text, solar_os_pwm_output_t *out)
{ (void)text;(void)out;return false; }
esp_err_t solar_os_pwm_open_output(const solar_os_pwm_output_t *o, uint32_t hz, const char *owner)
{ (void)o;(void)hz;(void)owner;return ESP_ERR_NOT_SUPPORTED; }
esp_err_t solar_os_pwm_write_output(const solar_os_pwm_output_t *o, const char *owner, uint32_t value)
{ (void)o;(void)owner;(void)value;return ESP_ERR_NOT_SUPPORTED; }
esp_err_t solar_os_pwm_close_output(const solar_os_pwm_output_t *o, const char *owner)
{ (void)o;(void)owner;return ESP_ERR_NOT_SUPPORTED; }
