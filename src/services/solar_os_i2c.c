#include "solar_os_i2c.h"

#include "solar_os_board_caps.h"
#if SOLAR_OS_BOARD_HAS_I2C
#include "i2c_bus.h"
#endif

esp_err_t solar_os_i2c_init(void)
{
#if SOLAR_OS_BOARD_HAS_I2C
    return i2c_bus_init();
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

uint32_t solar_os_i2c_get_speed_hz(void)
{
#if SOLAR_OS_BOARD_HAS_I2C
    return i2c_bus_get_speed_hz();
#else
    return 0;
#endif
}

int solar_os_i2c_get_sda_pin(void)
{
#if SOLAR_OS_BOARD_HAS_I2C
    return (int)i2c_bus_get_sda_pin();
#else
    return -1;
#endif
}

int solar_os_i2c_get_scl_pin(void)
{
#if SOLAR_OS_BOARD_HAS_I2C
    return (int)i2c_bus_get_scl_pin();
#else
    return -1;
#endif
}

esp_err_t solar_os_i2c_probe(uint8_t address)
{
#if SOLAR_OS_BOARD_HAS_I2C
    return i2c_bus_probe(address);
#else
    (void)address;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t solar_os_i2c_read_reg(uint8_t address, uint8_t reg, uint8_t *data, size_t len)
{
#if SOLAR_OS_BOARD_HAS_I2C
    return i2c_bus_read_reg(address, reg, data, len);
#else
    (void)address;
    (void)reg;
    (void)data;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t solar_os_i2c_write_reg(uint8_t address, uint8_t reg, const uint8_t *data, size_t len)
{
#if SOLAR_OS_BOARD_HAS_I2C
    return i2c_bus_write_reg(address, reg, data, len);
#else
    (void)address;
    (void)reg;
    (void)data;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
