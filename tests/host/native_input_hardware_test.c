#include <assert.h>
#include <string.h>

#include "adc_port.h"
#include "solar_os_adc_dpad.h"
#include "solar_os_input.h"
#include "solar_os_log.h"
#include "solar_os_native_input_hardware.h"

static int raw[2];
static bool second_axis_fails;
static unsigned presses, releases;
static solar_os_input_source_t source = 7;

bool adc_port_is_adc_capable(gpio_num_t pin, adc_unit_t *unit, adc_channel_t *channel)
{
    assert(pin == 4 || pin == 5);
    *unit = 0;
    *channel = pin;
    return true;
}

esp_err_t adc_port_configure_pin(gpio_num_t pin, int attenuation, int bits)
{
    assert((pin == 4 || pin == 5) && attenuation == ADC_ATTEN_DB_12 && bits == ADC_BITWIDTH_12);
    return ESP_OK;
}

esp_err_t adc_port_read(gpio_num_t pin, adc_port_sample_t *sample)
{
    assert(pin == 4 || pin == 5);
    if (pin == 5 && second_axis_fails) return ESP_ERR_TIMEOUT;
    sample->raw = raw[pin - 4];
    return ESP_OK;
}

esp_err_t solar_os_input_key_source_open(const char *name, solar_os_input_source_class_t kind,
                                         solar_os_input_source_t *out)
{
    assert(!strcmp(name, "adc-dpad") && kind == SOLAR_OS_INPUT_SOURCE_DPAD);
    *out = source;
    return ESP_OK;
}

esp_err_t solar_os_input_write_key(solar_os_input_source_t sender, uint16_t physical,
                                   uint16_t usage, uint8_t key, uint8_t modifiers,
                                   solar_os_input_key_action_t action)
{
    assert(sender == source && physical == 1 && usage == SOLAR_OS_INPUT_USAGE_NONE);
    assert(key == 1 && modifiers == 0);
    if (action == SOLAR_OS_INPUT_KEY_PRESS) presses++;
    else {
        assert(action == SOLAR_OS_INPUT_KEY_RELEASE);
        releases++;
    }
    return ESP_OK;
}

esp_err_t solar_os_log_write(solar_os_log_level_t level, const char *tag, const char *format, ...)
{
    assert(level == SOLAR_OS_LOG_LEVEL_INFO && !strcmp(tag, "solar_os_adc_dpad") && format);
    return ESP_OK;
}

int main(void)
{
    const solar_os_native_input_hardware_api_v1_t *api =
        solar_os_native_input_hardware_get_service("input.hardware", 1, sizeof(*api));
    assert(api && api->dpad_axis_count() == 2 && !api->button_count());
    assert(api->calibrate_dpad(false) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!solar_os_adc_dpad_init());

    raw[0] = 1300;
    solar_os_adc_dpad_poll();
    assert(presses == 1 && releases == 0);
    solar_os_native_dpad_axis_status_v1_t axis;
    assert(!api->dpad_axis_at(0, &axis) && axis.initialized && axis.raw_valid);
    assert(axis.zone == SOLAR_OS_NATIVE_DPAD_MID && axis.raw == 1300 && axis.idle_max == 100);
    assert(!strcmp(axis.name, "horizontal") && axis.pin == 4);
    assert(axis.mid_key == 1 && axis.high_key == 2 && axis.high_min == 3000);
    assert(api->dpad_axis_at(2, &axis) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !axis.name[0]);

    raw[0] = 200;
    raw[1] = 410;
    second_axis_fails = true;
    assert(api->calibrate_dpad(false) == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    assert(!api->dpad_axis_at(0, &axis) && axis.idle_max == 100 && !releases);
    assert(!api->dpad_axis_at(1, &axis) && !axis.raw_valid && axis.read_error == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    second_axis_fails = false;
    assert(!api->calibrate_dpad(false) && releases == 1);
    assert(!api->dpad_axis_at(0, &axis) && axis.idle_max == 500 && axis.zone == SOLAR_OS_NATIVE_DPAD_IDLE);
    assert(!api->dpad_axis_at(1, &axis) && axis.idle_max == 710);
    assert(!api->calibrate_dpad(true));
    assert(!api->dpad_axis_at(0, &axis) && axis.idle_max == 100);
    assert(!api->dpad_axis_at(1, &axis) && axis.idle_max == 120 && releases == 1);
    return 0;
}
