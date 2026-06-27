#include "solar_os_board_sensors.h"

#include "shtc3.h"

esp_err_t solar_os_board_sensors_init(void)
{
    return shtc3_init();
}

esp_err_t solar_os_board_sensors_read_environment(solar_os_board_environment_t *environment)
{
    if (environment == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    shtc3_measurement_t measurement;
    const esp_err_t err = shtc3_read_measurement(&measurement);
    if (err != ESP_OK) {
        return err;
    }

    environment->temperature_c = measurement.temperature_c;
    environment->humidity_percent = measurement.humidity_percent;
    return ESP_OK;
}

