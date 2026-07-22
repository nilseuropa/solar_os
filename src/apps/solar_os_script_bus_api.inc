/*
 * Shared Python/Lua registration descriptor for the bus and expansion APIs.
 *
 * Define SOLAR_OS_SCRIPT_API_MODULE_BEGIN, SOLAR_OS_SCRIPT_API_INT,
 * SOLAR_OS_SCRIPT_API_UINT, SOLAR_OS_SCRIPT_API_FUNCTION, and
 * SOLAR_OS_SCRIPT_API_MODULE_END before including this file. This file is
 * intentionally re-includable so each interpreter can generate its native
 * registration calls while keeping argument conversion in its own binding.
 */

#ifndef SOLAR_OS_SCRIPT_API_MODULE_BEGIN
#error "SOLAR_OS_SCRIPT_API_MODULE_BEGIN must be defined"
#endif
#ifndef SOLAR_OS_SCRIPT_API_INT
#error "SOLAR_OS_SCRIPT_API_INT must be defined"
#endif
#ifndef SOLAR_OS_SCRIPT_API_UINT
#error "SOLAR_OS_SCRIPT_API_UINT must be defined"
#endif
#ifndef SOLAR_OS_SCRIPT_API_FUNCTION
#error "SOLAR_OS_SCRIPT_API_FUNCTION must be defined"
#endif
#ifndef SOLAR_OS_SCRIPT_API_MODULE_END
#error "SOLAR_OS_SCRIPT_API_MODULE_END must be defined"
#endif

#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
SOLAR_OS_SCRIPT_API_MODULE_BEGIN(buses);

SOLAR_OS_SCRIPT_API_FUNCTION(buses, list, list);
SOLAR_OS_SCRIPT_API_FUNCTION(buses, get, get);
SOLAR_OS_SCRIPT_API_FUNCTION(buses, remove, remove);
SOLAR_OS_SCRIPT_API_FUNCTION(buses, attach, attach);
SOLAR_OS_SCRIPT_API_FUNCTION(buses, detach, detach);

#if SOLAR_OS_PACKAGE_SERVICE_SPI
SOLAR_OS_SCRIPT_API_INT(buses, MODE0, 0);
SOLAR_OS_SCRIPT_API_INT(buses, MODE1, 1);
SOLAR_OS_SCRIPT_API_INT(buses, MODE2, 2);
SOLAR_OS_SCRIPT_API_INT(buses, MODE3, 3);
SOLAR_OS_SCRIPT_API_INT(buses, SPI2_HOST, SPI2_HOST);
SOLAR_OS_SCRIPT_API_INT(buses, SPI3_HOST, SPI3_HOST);
SOLAR_OS_SCRIPT_API_UINT(buses, DEFAULT_SPEED, SOLAR_OS_BUS_SPI_DEFAULT_SPEED_HZ);
SOLAR_OS_SCRIPT_API_UINT(buses, MAX_SPEED, SOLAR_OS_BUS_SPI_MAX_SPEED_HZ);
SOLAR_OS_SCRIPT_API_FUNCTION(buses, create_spi, create_spi);
SOLAR_OS_SCRIPT_API_FUNCTION(buses, spi_xfer, spi_xfer);
SOLAR_OS_SCRIPT_API_FUNCTION(buses, spi_read, spi_read);
SOLAR_OS_SCRIPT_API_FUNCTION(buses, spi_write, spi_write);
#endif

#if SOLAR_OS_PACKAGE_SERVICE_I2C
SOLAR_OS_SCRIPT_API_FUNCTION(buses, create_i2c, create_i2c);
SOLAR_OS_SCRIPT_API_FUNCTION(buses, i2c_probe, i2c_probe);
SOLAR_OS_SCRIPT_API_FUNCTION(buses, i2c_scan, i2c_scan);
SOLAR_OS_SCRIPT_API_FUNCTION(buses, i2c_read_reg, i2c_read_reg);
SOLAR_OS_SCRIPT_API_FUNCTION(buses, i2c_write_reg, i2c_write_reg);
#endif

#if SOLAR_OS_PACKAGE_SERVICE_ONEWIRE
SOLAR_OS_SCRIPT_API_FUNCTION(buses, create_onewire, create_onewire);
SOLAR_OS_SCRIPT_API_FUNCTION(buses, onewire_reset, onewire_reset);
SOLAR_OS_SCRIPT_API_FUNCTION(buses, onewire_scan, onewire_scan);
SOLAR_OS_SCRIPT_API_FUNCTION(buses, onewire_xfer, onewire_xfer);
#endif

#if SOLAR_OS_PACKAGE_SERVICE_UART
SOLAR_OS_SCRIPT_API_FUNCTION(buses, create_uart, create_uart);
SOLAR_OS_SCRIPT_API_FUNCTION(buses, uart_write, uart_write);
SOLAR_OS_SCRIPT_API_FUNCTION(buses, uart_read, uart_read);
#endif

SOLAR_OS_SCRIPT_API_MODULE_END(buses);
#endif

#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
SOLAR_OS_SCRIPT_API_MODULE_BEGIN(expansion);
SOLAR_OS_SCRIPT_API_FUNCTION(expansion, drivers, drivers);
SOLAR_OS_SCRIPT_API_FUNCTION(expansion, devices, devices);
SOLAR_OS_SCRIPT_API_FUNCTION(expansion, attach, attach);
SOLAR_OS_SCRIPT_API_FUNCTION(expansion, detach, detach);
SOLAR_OS_SCRIPT_API_MODULE_END(expansion);
#endif
