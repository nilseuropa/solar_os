#include "solar_os_sensors.h"

#include <stddef.h>

#include "solar_os_board_caps.h"
#if SOLAR_OS_BOARD_HAS_TEMPERATURE || SOLAR_OS_BOARD_HAS_HUMIDITY
#include "solar_os_board_sensors.h"
#endif

esp_err_t solar_os_sensors_init(void)
{
#if SOLAR_OS_BOARD_HAS_TEMPERATURE || SOLAR_OS_BOARD_HAS_HUMIDITY
    return solar_os_board_sensors_init();
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
    solar_os_board_environment_t measurement;
    const esp_err_t ret = solar_os_board_sensors_read_environment(&measurement);
    if (ret != ESP_OK) {
        return ret;
    }

    environment->temperature_c = measurement.temperature_c;
    environment->humidity_percent = measurement.humidity_percent;
    return ESP_OK;
#endif
}
