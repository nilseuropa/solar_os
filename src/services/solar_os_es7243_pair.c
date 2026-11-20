#include "audio_codec_board.h"
#include "solar_os_buses.h"
#include "solar_os_expansion.h"
#include <string.h>

static esp_err_t attach(const char *name, const solar_os_expansion_binding_t *b,
    size_t count, audio_codec_board_mode_t mode)
{
    if (!b || !name || !name[0]) return ESP_ERR_INVALID_ARG;
    audio_codec_board_config_t c = {.mode = mode, .i2s_port = -1, .i2c_port = -1,
        .mclk_pin = -1, .bclk_pin = -1, .ws_pin = -1, .din_pin = -1, .dout_pin = -1, .pa_pin = -1};
    const char *bus = NULL, *adc_bus = NULL, *dac_bus = NULL;
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = 0; j < i; ++j)
            if (b[i].kind == b[j].kind && !strcmp(b[i].role, b[j].role)) return ESP_ERR_INVALID_ARG;
        if (b[i].kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS) bus = b[i].target;
        else if (b[i].kind == SOLAR_OS_EXPANSION_BINDING_I2S_PORT) c.i2s_port = b[i].value;
        else if (b[i].kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS) {
            if (b[i].value < 0x08 || b[i].value > 0x77) return ESP_ERR_INVALID_ARG;
            if (!b[i].role[0]) { c.input_address = b[i].value; adc_bus = b[i].target; }
            else if (!strcmp(b[i].role, "dac_addr")) { c.output_address = b[i].value; dac_bus = b[i].target; }
            else return ESP_ERR_INVALID_ARG;
        } else if (b[i].kind == SOLAR_OS_EXPANSION_BINDING_GPIO) {
            int *pin = NULL;
            if (!strcmp(b[i].role, "mclk")) pin = &c.mclk_pin;
            else if (!strcmp(b[i].role, "bck")) pin = &c.bclk_pin;
            else if (!strcmp(b[i].role, "ws")) pin = &c.ws_pin;
            else if (!strcmp(b[i].role, "din")) pin = &c.din_pin;
            else if (!strcmp(b[i].role, "dout")) pin = &c.dout_pin;
            if (!pin) return ESP_ERR_INVALID_ARG;
            *pin = b[i].value;
        } else if (b[i].kind == SOLAR_OS_EXPANSION_BINDING_GPIO_LINE && !strcmp(b[i].role, "pa")) {
            if (b[i].value < 0 || b[i].value > UINT8_MAX ||
                strlcpy(c.pa_line.controller, b[i].target, sizeof(c.pa_line.controller)) >= sizeof(c.pa_line.controller))
                return ESP_ERR_INVALID_ARG;
            c.pa_line.line = b[i].value; c.have_pa_line = true;
        } else return ESP_ERR_INVALID_ARG;
    }
    if (!bus || !adc_bus || !dac_bus || !c.input_address || !c.output_address ||
        c.input_address == c.output_address || (adc_bus[0] && strcmp(bus, adc_bus)) ||
        (dac_bus[0] && strcmp(bus, dac_bus))) return ESP_ERR_INVALID_ARG;
    const int pins[] = {c.mclk_pin, c.bclk_pin, c.ws_pin, c.din_pin, c.dout_pin,
        c.have_pa_line && !c.pa_line.controller[0] ? c.pa_line.line : -1};
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        if (i < 5 && pins[i] < 0) return ESP_ERR_INVALID_ARG;
        for (size_t j = i + 1; j < sizeof(pins) / sizeof(pins[0]); ++j)
            if (pins[i] >= 0 && pins[i] == pins[j]) return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = solar_os_bus_i2c_get_handle(bus, &c.i2c_handle, &c.i2c_port);
    return error ? error : audio_codec_board_attach(name, &c);
}
static esp_err_t attach_es7243(const char *name, const solar_os_expansion_binding_t *b, size_t count)
{ return attach(name, b, count, AUDIO_CODEC_BOARD_ES8311_ES7243); }
static esp_err_t attach_es7243e(const char *name, const solar_os_expansion_binding_t *b, size_t count)
{ return attach(name, b, count, AUDIO_CODEC_BOARD_ES8311_ES7243E); }

#define PIN(key_) {.key = key_, .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = key_, .required = true}
static const solar_os_expansion_binding_spec_t bindings[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "addr", .value_hint = "ADC 7-bit address", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .required = true, .has_value_range = true, .min_value = 0x08, .max_value = 0x77},
    {.key = "dac_addr", .value_hint = "ES8311 7-bit address", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .role = "dac_addr", .required = true, .has_value_range = true, .min_value = 0x08, .max_value = 0x77},
    {.key = "i2s", .value_hint = "i2s0|i2s1", .kind = SOLAR_OS_EXPANSION_BINDING_I2S_PORT, .required = true},
    PIN("mclk"), PIN("bck"), PIN("ws"), PIN("din"), PIN("dout"),
    {.key = "pa", .value_hint = "gpio|controller:line", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE, .role = "pa"},
};
#undef PIN
const solar_os_expansion_driver_t solar_os_es8311_es7243_expansion_driver = {
    .name = "es8311-es7243", .category = SOLAR_OS_EXPANSION_CATEGORY_AUDIO,
    .summary = "ES8311 output and ES7243 input on shared I2S",
    .required_capabilities = SOLAR_OS_BOARD_CAP_I2C | SOLAR_OS_BOARD_CAP_EXPANSION_I2S,
    .binding_specs = bindings, .binding_spec_count = sizeof(bindings) / sizeof(bindings[0]),
    .attach = attach_es7243, .detach = audio_codec_board_detach,
};
const solar_os_expansion_driver_t solar_os_es8311_es7243e_expansion_driver = {
    .name = "es8311-es7243e", .category = SOLAR_OS_EXPANSION_CATEGORY_AUDIO,
    .summary = "ES8311 output and ES7243E input on shared I2S",
    .required_capabilities = SOLAR_OS_BOARD_CAP_I2C | SOLAR_OS_BOARD_CAP_EXPANSION_I2S,
    .binding_specs = bindings, .binding_spec_count = sizeof(bindings) / sizeof(bindings[0]),
    .attach = attach_es7243e, .detach = audio_codec_board_detach,
};
