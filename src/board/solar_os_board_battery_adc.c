#include "solar_os_board_battery.h"

#include "battery_adc.h"

esp_err_t solar_os_board_battery_init(void)
{
    return battery_adc_init();
}

esp_err_t solar_os_board_battery_read(solar_os_board_battery_sample_t *sample)
{
    if (sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    battery_adc_sample_t driver_sample;
    const esp_err_t err = battery_adc_read(&driver_sample);
    if (err != ESP_OK) {
        return err;
    }

    sample->battery_mv = driver_sample.battery_mv;
    sample->calibrated = driver_sample.calibrated;
    return ESP_OK;
}
