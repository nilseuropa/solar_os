#include "solar_os_spi.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_buses.h"

#define SOLAR_OS_SPI_COMPAT_BUS "spi0"
#define SOLAR_OS_SPI_COMPAT_OWNER "legacy-spi"

static bool default_bus(solar_os_bus_info_t *info)
{
    if (solar_os_bus_find(SOLAR_OS_SPI_COMPAT_BUS, SOLAR_OS_BUS_PROTOCOL_SPI, info)) {
        return true;
    }
    return solar_os_bus_get_protocol(SOLAR_OS_BUS_PROTOCOL_SPI, 0, info);
}

esp_err_t solar_os_spi_init(void)
{
    return solar_os_buses_init();
}

esp_err_t solar_os_spi_get_status(solar_os_spi_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(status, 0, sizeof(*status));
    status->default_speed_hz = SOLAR_OS_SPI_DEFAULT_SPEED_HZ;

    solar_os_bus_info_t info;
    if (!default_bus(&info)) {
        return ESP_OK;
    }

    status->available = true;
    status->host = info.config.spi.host;
    strlcpy(status->name, info.name, sizeof(status->name));
    status->sclk_pin = info.config.spi.sclk_pin;
    status->miso_pin = info.config.spi.miso_pin;
    status->mosi_pin = info.config.spi.mosi_pin;
    status->max_transfer_size = info.config.spi.max_transfer_size;
    status->cs_count = info.config.spi.cs_count;
    for (size_t i = 0; i < status->cs_count; i++) {
        status->cs[i].pin = info.config.spi.cs[i].pin;
        strlcpy(status->cs[i].name,
                info.config.spi.cs[i].name,
                sizeof(status->cs[i].name));
    }
    return ESP_OK;
}

esp_err_t solar_os_spi_resolve_cs(const char *name, int *pin)
{
    if (name == NULL || name[0] == '\0' || pin == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_bus_info_t info;
    if (!default_bus(&info)) {
        return ESP_ERR_NOT_FOUND;
    }
    for (size_t i = 0; i < info.config.spi.cs_count; i++) {
        if (strcmp(name, info.config.spi.cs[i].name) == 0) {
            *pin = info.config.spi.cs[i].pin;
            return ESP_OK;
        }
    }

    char *end = NULL;
    errno = 0;
    const long parsed = strtol(name, &end, 0);
    if (errno != 0 || end == name || *end != '\0' || parsed < 0) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < info.config.spi.cs_count; i++) {
        if (parsed == info.config.spi.cs[i].pin) {
            *pin = (int)parsed;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_spi_transfer(int cs_pin,
                                uint8_t mode,
                                uint32_t speed_hz,
                                const uint8_t *tx_data,
                                uint8_t *rx_data,
                                size_t len)
{
    solar_os_bus_info_t info;
    if (!default_bus(&info)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (speed_hz == 0) {
        speed_hz = SOLAR_OS_SPI_DEFAULT_SPEED_HZ;
    }
    if (mode > 3 || speed_hz > SOLAR_OS_SPI_MAX_SPEED_HZ || len == 0 ||
        len > info.config.spi.max_transfer_size ||
        (tx_data == NULL && rx_data == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    return solar_os_bus_spi_transfer_once(info.name,
                                          cs_pin,
                                          mode,
                                          speed_hz,
                                          tx_data,
                                          rx_data,
                                          len,
                                          SOLAR_OS_SPI_COMPAT_OWNER);
}
