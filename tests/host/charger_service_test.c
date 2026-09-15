#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_charger.h"

typedef struct { solar_os_charger_status_t status; } fake_charger_t;

static esp_err_t read_status(void *ctx, solar_os_charger_status_t *status)
{
    *status = ((fake_charger_t *)ctx)->status;
    return ESP_OK;
}

static esp_err_t set_enabled(void *ctx, bool value)
{
    ((fake_charger_t *)ctx)->status.enabled = value;
    return ESP_OK;
}

static esp_err_t set_input(void *ctx, uint16_t value)
{
    ((fake_charger_t *)ctx)->status.input_current_limit_ma = value;
    return ESP_OK;
}

static esp_err_t set_current(void *ctx, uint16_t value)
{
    ((fake_charger_t *)ctx)->status.charge_current_ma = value;
    return ESP_OK;
}

static esp_err_t set_voltage(void *ctx, uint16_t value)
{
    ((fake_charger_t *)ctx)->status.charge_voltage_mv = value;
    return ESP_OK;
}

int main(void)
{
    static const solar_os_charger_ops_t ops = {
        .read_status = read_status,
        .set_enabled = set_enabled,
        .set_input_current_limit = set_input,
        .set_charge_current = set_current,
        .set_charge_voltage = set_voltage,
    };
    fake_charger_t fake = {0};
    const solar_os_charger_registration_t registration = {
        .name = "charger0",
        .driver = "fake",
        .input_current_limit_ma = {100, 500, 50},
        .charge_current_ma = {0, 1024, 64},
        .charge_voltage_mv = {3840, 4352, 16},
        .ops = &ops,
        .ctx = &fake,
    };
    assert(solar_os_charger_register(&registration) == ESP_OK);
    assert(solar_os_charger_register(&registration) == ESP_ERR_INVALID_STATE);
    assert(solar_os_charger_count() == 1U);
    solar_os_charger_info_t info;
    assert(solar_os_charger_get(0U, &info));
    assert(strcmp(info.name, "charger0") == 0);
    assert(info.charge_current_ma.step == 64U);

    assert(solar_os_charger_set_enabled("charger0", true) == ESP_OK);
    assert(solar_os_charger_set_input_current_limit("charger0", 350U) == ESP_OK);
    assert(solar_os_charger_set_input_current_limit("charger0", 351U) == ESP_ERR_INVALID_ARG);
    assert(solar_os_charger_set_charge_current("charger0", 704U) == ESP_OK);
    assert(solar_os_charger_set_charge_current("charger0", 705U) == ESP_ERR_INVALID_ARG);
    assert(solar_os_charger_set_charge_voltage("charger0", 4288U) == ESP_OK);
    assert(solar_os_charger_set_charge_voltage("charger0", 4700U) == ESP_ERR_INVALID_ARG);

    solar_os_charger_status_t status;
    assert(solar_os_charger_read_status("charger0", &status) == ESP_OK);
    assert(status.enabled);
    assert(status.input_current_limit_ma == 350U);
    assert(status.charge_current_ma == 704U);
    assert(status.charge_voltage_mv == 4288U);
    assert(strcmp(solar_os_charger_state_name(SOLAR_OS_CHARGER_STATE_FAST_CHARGE),
                  "fast-charge") == 0);
    assert(solar_os_charger_unregister("charger0") == ESP_OK);
    assert(solar_os_charger_count() == 0U);
    puts("charger service registry tests: ok");
    return 0;
}
