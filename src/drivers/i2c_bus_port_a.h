#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

/*
 * Grove Port A's external I2C bus, shared by every driver/app that
 * talks to a device connected there (CardKB, the aqm app's SCD41 CO2
 * sensor, whatever comes next). Its own i2c_master instance, separate
 * from the board's internal bus (i2c_bus.c) -- Port A is physically a
 * different I2C controller on these boards, not a switched extension
 * of the internal one.
 *
 * i2c_new_master_bus() can only be called once per port, so this is
 * the single owner of that call; callers just add their own device
 * (i2c_master_bus_add_device()) on the handle this returns.
 */
esp_err_t i2c_bus_port_a_init(void);
i2c_master_bus_handle_t i2c_bus_port_a_get_handle(void);
void i2c_bus_port_a_lock(void);
void i2c_bus_port_a_unlock(void);
