#include "solar_os_i2c.h"

#include <string.h>

#include "solar_os_board_caps.h"
#include "solar_os_config.h"
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
#include "solar_os_buses.h"
#endif
#if SOLAR_OS_BOARD_HAS_I2C
#include "i2c_bus.h"
#endif

#define I2C_SERVICE_OWNER "service-i2c"

static esp_err_t i2c_finish_operation(const char *name, esp_err_t operation_err)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    const esp_err_t release_err = solar_os_bus_release(name,
                                                       SOLAR_OS_BUS_PROTOCOL_I2C,
                                                       I2C_SERVICE_OWNER);
    return operation_err == ESP_OK ? release_err : operation_err;
#else
    (void)name;
    return operation_err;
#endif
}

esp_err_t solar_os_i2c_init(void)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    if (!solar_os_bus_find(SOLAR_OS_I2C_DEFAULT_BUS,
                           SOLAR_OS_BUS_PROTOCOL_I2C,
                           NULL)) {
        return SOLAR_OS_BOARD_HAS_EXPANSION_I2C ? ESP_OK : ESP_ERR_NOT_FOUND;
    }
    esp_err_t ret = solar_os_bus_acquire(SOLAR_OS_I2C_DEFAULT_BUS,
                                         SOLAR_OS_BUS_PROTOCOL_I2C,
                                         I2C_SERVICE_OWNER);
    return ret == ESP_OK
        ? i2c_finish_operation(SOLAR_OS_I2C_DEFAULT_BUS, ESP_OK)
        : ret;
#elif SOLAR_OS_BOARD_HAS_I2C
    return i2c_bus_init();
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

uint32_t solar_os_i2c_get_speed_hz(void)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    solar_os_bus_info_t info;
    return solar_os_bus_find(SOLAR_OS_I2C_DEFAULT_BUS,
                             SOLAR_OS_BUS_PROTOCOL_I2C,
                             &info)
        ? info.config.i2c.speed_hz
        : 0;
#elif SOLAR_OS_BOARD_HAS_I2C
    return i2c_bus_get_speed_hz();
#else
    return 0;
#endif
}

int solar_os_i2c_get_sda_pin(void)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    solar_os_bus_info_t info;
    return solar_os_bus_find(SOLAR_OS_I2C_DEFAULT_BUS,
                             SOLAR_OS_BUS_PROTOCOL_I2C,
                             &info)
        ? info.config.i2c.sda_pin
        : -1;
#elif SOLAR_OS_BOARD_HAS_I2C
    return (int)i2c_bus_get_sda_pin();
#else
    return -1;
#endif
}

int solar_os_i2c_get_scl_pin(void)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    solar_os_bus_info_t info;
    return solar_os_bus_find(SOLAR_OS_I2C_DEFAULT_BUS,
                             SOLAR_OS_BUS_PROTOCOL_I2C,
                             &info)
        ? info.config.i2c.scl_pin
        : -1;
#elif SOLAR_OS_BOARD_HAS_I2C
    return (int)i2c_bus_get_scl_pin();
#else
    return -1;
#endif
}

esp_err_t solar_os_i2c_bus_probe(const char *name, uint8_t address)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    esp_err_t ret = solar_os_bus_acquire(name,
                                         SOLAR_OS_BUS_PROTOCOL_I2C,
                                         I2C_SERVICE_OWNER);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = solar_os_bus_i2c_probe(name, address);
    return i2c_finish_operation(name, ret);
#elif SOLAR_OS_BOARD_HAS_I2C
    if (name == NULL || strcmp(name, SOLAR_OS_I2C_DEFAULT_BUS) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    return i2c_bus_probe(address);
#else
    (void)name;
    (void)address;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t solar_os_i2c_bus_read_reg(const char *name,
                                    uint8_t address,
                                    uint8_t reg,
                                    uint8_t *data,
                                    size_t len)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    esp_err_t ret = solar_os_bus_acquire(name,
                                         SOLAR_OS_BUS_PROTOCOL_I2C,
                                         I2C_SERVICE_OWNER);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = solar_os_bus_i2c_read_reg(name, address, reg, data, len);
    return i2c_finish_operation(name, ret);
#elif SOLAR_OS_BOARD_HAS_I2C
    if (name == NULL || strcmp(name, SOLAR_OS_I2C_DEFAULT_BUS) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    return i2c_bus_read_reg(address, reg, data, len);
#else
    (void)name;
    (void)address;
    (void)reg;
    (void)data;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t solar_os_i2c_bus_write_reg(const char *name,
                                     uint8_t address,
                                     uint8_t reg,
                                     const uint8_t *data,
                                     size_t len)
{
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
    esp_err_t ret = solar_os_bus_acquire(name,
                                         SOLAR_OS_BUS_PROTOCOL_I2C,
                                         I2C_SERVICE_OWNER);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = solar_os_bus_i2c_write_reg(name, address, reg, data, len);
    return i2c_finish_operation(name, ret);
#elif SOLAR_OS_BOARD_HAS_I2C
    if (name == NULL || strcmp(name, SOLAR_OS_I2C_DEFAULT_BUS) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    return i2c_bus_write_reg(address, reg, data, len);
#else
    (void)name;
    (void)address;
    (void)reg;
    (void)data;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t solar_os_i2c_probe(uint8_t address)
{
    return solar_os_i2c_bus_probe(SOLAR_OS_I2C_DEFAULT_BUS, address);
}

esp_err_t solar_os_i2c_read_reg(uint8_t address, uint8_t reg, uint8_t *data, size_t len)
{
    return solar_os_i2c_bus_read_reg(SOLAR_OS_I2C_DEFAULT_BUS,
                                     address,
                                     reg,
                                     data,
                                     len);
}

esp_err_t solar_os_i2c_write_reg(uint8_t address,
                                 uint8_t reg,
                                 const uint8_t *data,
                                 size_t len)
{
    return solar_os_i2c_bus_write_reg(SOLAR_OS_I2C_DEFAULT_BUS,
                                      address,
                                      reg,
                                      data,
                                      len);
}
