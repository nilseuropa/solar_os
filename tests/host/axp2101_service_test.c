#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_axp2101.h"
#include "solar_os_battery.h"
#include "solar_os_buses.h"
#include "solar_os_charger.h"
#include "solar_os_power.h"
#include "freertos/semphr.h"

/* Exercise the service and real register driver with a simulated PMIC. */
static uint8_t registers[256];
static esp_err_t write_result = ESP_OK;
static unsigned unregister_calls;
static bool charger_registered, battery_registered, power_busy;
static solar_os_power_off_fn_t off_callback;
static void *off_user;

esp_err_t solar_os_power_register_off_provider(const char *name,
                                               solar_os_power_off_fn_t off, void *user)
{
    assert(!strcmp(name, "power0"));
    off_callback = off; off_user = user;
    return ESP_OK;
}
esp_err_t solar_os_power_unregister_off_provider(void *user)
{
    if (off_user != user) return ESP_ERR_NOT_FOUND;
    if (power_busy) return ESP_ERR_INVALID_STATE;
    off_callback = NULL;
    off_user = NULL;
    return ESP_OK;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t len = strlen(src);
    if (size > 0U) {
        const size_t copy = len < size - 1U ? len : size - 1U;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore) { (void)semaphore; }

bool solar_os_expansion_find_i2c_bus(const char *name,
                                     solar_os_expansion_i2c_bus_t *bus,
                                     size_t *index)
{
    (void)bus;
    (void)index;
    return strcmp(name, "i2c0") == 0;
}

esp_err_t solar_os_bus_i2c_read_reg(const char *name, uint8_t address,
                                    uint8_t reg, uint8_t *data, size_t len)
{
    assert(strcmp(name, "i2c0") == 0 && address == 0x34U);
    assert((size_t)reg + len <= sizeof(registers));
    memcpy(data, registers + reg, len);
    return ESP_OK;
}

esp_err_t solar_os_bus_i2c_write_reg(const char *name, uint8_t address,
                                     uint8_t reg, const uint8_t *data, size_t len)
{
    assert(strcmp(name, "i2c0") == 0 && address == 0x34U);
    assert((size_t)reg + len <= sizeof(registers));
    if (write_result == ESP_OK) memcpy(registers + reg, data, len);
    return write_result;
}

esp_err_t solar_os_battery_register_provider(const char *owner,
                                             const solar_os_battery_provider_t *provider)
{
    assert(strcmp(owner, "power0") == 0 && provider->read != NULL);
    assert(!battery_registered);
    battery_registered = true;
    return ESP_OK;
}

esp_err_t solar_os_battery_unregister_provider(const char *owner)
{
    assert(strcmp(owner, "power0") == 0);
    if (!battery_registered) return ESP_ERR_NOT_FOUND;
    battery_registered = false;
    unregister_calls++;
    return ESP_OK;
}

esp_err_t solar_os_battery_unregister_provider_owned(const char *owner, const void *user)
{
    assert(off_user == user);
    return solar_os_battery_unregister_provider(owner);
}

esp_err_t solar_os_charger_register(const solar_os_charger_registration_t *registration)
{
    assert(strcmp(registration->name, "charger0") == 0);
    assert(!charger_registered);
    charger_registered = true;
    return ESP_OK;
}

esp_err_t solar_os_charger_unregister_owned(const char *name, const void *ctx)
{
    assert(strcmp(name, "charger0") == 0 && off_user == ctx);
    if (!charger_registered) return ESP_ERR_NOT_FOUND;
    charger_registered = false;
    unregister_calls++;
    return ESP_OK;
}

int main(void)
{
    const solar_os_expansion_binding_t bindings[] = {
        {.kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .target = "i2c0"},
        {.kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .value = 0x34},
    };
    assert(solar_os_axp2101_acquire_aldo3(NULL, 0x34U) == ESP_ERR_INVALID_ARG);
    assert(solar_os_axp2101_acquire_aldo3("i2c0", 0x34U) == ESP_ERR_NOT_FOUND);
    assert(solar_os_axp2101_set_aldo3(true) == ESP_ERR_INVALID_STATE);
    registers[0x03] = 0x4AU;
    assert(solar_os_axp2101_attach("power0", bindings, 2U) == ESP_OK);
    registers[0x10] = 0xA4U;
    assert(off_callback != NULL && off_callback(off_user) == ESP_OK);
    assert(registers[0x10] == 0xA5U);
    assert(solar_os_axp2101_acquire_aldo3("i2c1", 0x34U) == ESP_ERR_NOT_FOUND);
    assert(solar_os_axp2101_acquire_aldo3("i2c0", 0x35U) == ESP_ERR_NOT_FOUND);
    assert(solar_os_axp2101_acquire_aldo3("i2c0", 0x34U) == ESP_OK);
    assert(solar_os_axp2101_acquire_aldo3("i2c0", 0x34U) == ESP_ERR_INVALID_STATE);
    assert(solar_os_axp2101_detach("power0") == ESP_ERR_INVALID_STATE);
    assert(unregister_calls == 0U);
    registers[0x90] = 0xA0U;
    registers[0x94] = 0xE0U;
    assert(solar_os_axp2101_set_aldo3(true) == ESP_OK);
    assert(registers[0x90] == 0xA4U && registers[0x94] == 0xFCU);
    write_result = ESP_ERR_TIMEOUT;
    assert(solar_os_axp2101_set_aldo3(false) == ESP_ERR_TIMEOUT);
    write_result = ESP_OK;
    assert(solar_os_axp2101_set_aldo3(false) == ESP_OK);
    assert(registers[0x90] == 0xA0U);
    solar_os_axp2101_release_aldo3();
    assert(solar_os_axp2101_set_aldo3(true) == ESP_ERR_INVALID_STATE);
    power_busy = true;
    assert(solar_os_axp2101_detach("power0") == ESP_ERR_INVALID_STATE);
    assert(off_callback && unregister_calls == 2U);
    power_busy = false;
    assert(solar_os_axp2101_detach("power0") == ESP_OK);
    assert(!off_callback && unregister_calls == 2U);
    assert(solar_os_axp2101_attach("power0", bindings, 2U) == ESP_OK);
    assert(solar_os_axp2101_acquire_aldo3("i2c0", 0x34U) == ESP_OK);
    solar_os_axp2101_release_aldo3();
    assert(solar_os_axp2101_acquire_aldo3("i2c0", 0x34U) == ESP_OK);
    solar_os_axp2101_release_aldo3();
    assert(solar_os_axp2101_detach("power0") == ESP_OK);
    puts("AXP2101 service tests: ok");
    return 0;
}
