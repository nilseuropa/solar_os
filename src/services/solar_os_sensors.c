#include "solar_os_sensors.h"

#include <stddef.h>

#include "solar_os_board_caps.h"
#if SOLAR_OS_BOARD_HAS_TEMPERATURE || SOLAR_OS_BOARD_HAS_HUMIDITY
#include "shtc3.h"
#endif

esp_err_t solar_os_sensors_init(void)
{
#if SOLAR_OS_BOARD_HAS_TEMPERATURE || SOLAR_OS_BOARD_HAS_HUMIDITY
    return shtc3_init();
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t solar_os_sensors_read_environment(solar_os_environment_t *environment)
{
    if (environment == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#if !SOLAR_OS_BOARD_HAS_TEMPERATURE && !SOLAR_OS_BOARD_HAS_HUMIDITY
    return ESP_ERR_NOT_SUPPORTED;
#else
    shtc3_measurement_t measurement;
    const esp_err_t ret = shtc3_read_measurement(&measurement);
    if (ret != ESP_OK) {
        return ret;
    }

    environment->temperature_c = measurement.temperature_c;
    environment->humidity_percent = measurement.humidity_percent;
    return ESP_OK;
#endif
}
