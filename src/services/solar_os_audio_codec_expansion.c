#include "solar_os_audio_codec_expansion.h"

#include <string.h>

#include "audio_codec_board.h"
#include "solar_os_buses.h"

typedef struct {
    const char *i2c_bus;
    int i2s_port;
    int mclk_pin;
    int bclk_pin;
    int ws_pin;
    int din_pin;
    int dout_pin;
    int pa_pin;
} audio_codec_bindings_t;

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t count,
                                audio_codec_bindings_t *parsed)
{
    if (bindings == NULL || parsed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *parsed = (audio_codec_bindings_t) {
        .i2s_port = -1,
        .mclk_pin = -1,
        .bclk_pin = -1,
        .ws_pin = -1,
        .din_pin = -1,
        .dout_pin = -1,
        .pa_pin = -1,
    };
    for (size_t i = 0; i < count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS) {
            parsed->i2c_bus = binding->target;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2S_PORT) {
            parsed->i2s_port = binding->value;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO) {
            if (strcmp(binding->role, "mclk") == 0) parsed->mclk_pin = binding->value;
            else if (strcmp(binding->role, "bck") == 0) parsed->bclk_pin = binding->value;
            else if (strcmp(binding->role, "ws") == 0) parsed->ws_pin = binding->value;
            else if (strcmp(binding->role, "din") == 0) parsed->din_pin = binding->value;
            else if (strcmp(binding->role, "dout") == 0) parsed->dout_pin = binding->value;
            else if (strcmp(binding->role, "pa") == 0) parsed->pa_pin = binding->value;
        }
    }
    return parsed->i2c_bus != NULL && parsed->i2s_port >= 0 &&
        parsed->mclk_pin >= 0 && parsed->bclk_pin >= 0 &&
        parsed->ws_pin >= 0 && parsed->din_pin >= 0 &&
        parsed->dout_pin >= 0 ?
        ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t attach_codec(const char *name,
                              const solar_os_expansion_binding_t *bindings,
                              size_t count,
                              audio_codec_board_mode_t mode)
{
    audio_codec_bindings_t parsed;
    esp_err_t ret = parse_bindings(bindings, count, &parsed);
    if (ret != ESP_OK) {
        return ret;
    }
    i2c_master_bus_handle_t i2c_handle = NULL;
    int i2c_port = -1;
    ret = solar_os_bus_i2c_get_handle(parsed.i2c_bus, &i2c_handle, &i2c_port);
    if (ret != ESP_OK) {
        return ret;
    }
    const audio_codec_board_config_t config = {
        .mode = mode,
        .i2c_handle = i2c_handle,
        .i2c_port = i2c_port,
        .i2s_port = parsed.i2s_port,
        .mclk_pin = parsed.mclk_pin,
        .bclk_pin = parsed.bclk_pin,
        .ws_pin = parsed.ws_pin,
        .din_pin = parsed.din_pin,
        .dout_pin = parsed.dout_pin,
        .pa_pin = parsed.pa_pin,
    };
    return audio_codec_board_attach(name, &config);
}

static esp_err_t attach_es8311_es7210(
    const char *name, const solar_os_expansion_binding_t *bindings, size_t count)
{
    return attach_codec(name, bindings, count,
                        AUDIO_CODEC_BOARD_ES8311_ES7210);
}

static esp_err_t attach_es8311_duplex(
    const char *name, const solar_os_expansion_binding_t *bindings, size_t count)
{
    return attach_codec(name, bindings, count,
                        AUDIO_CODEC_BOARD_ES8311_DUPLEX);
}

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {.key = "i2s", .value_hint = "i2s0|i2s1", .kind = SOLAR_OS_EXPANSION_BINDING_I2S_PORT, .required = true},
    {.key = "mclk", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "mclk", .required = true},
    {.key = "bck", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "bck", .required = true},
    {.key = "ws", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "ws", .required = true},
    {.key = "din", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "din", .required = true},
    {.key = "dout", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "dout", .required = true},
    {.key = "pa", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "pa", .required = false},
};

const solar_os_expansion_driver_t solar_os_es8311_es7210_expansion_driver = {
    .name = "es8311-es7210",
    .summary = "ES8311 output + ES7210 input",
    .required_capabilities = SOLAR_OS_BOARD_CAP_I2C |
                             SOLAR_OS_BOARD_CAP_EXPANSION_I2S,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = attach_es8311_es7210,
    .detach = audio_codec_board_detach,
};

const solar_os_expansion_driver_t solar_os_es8311_duplex_expansion_driver = {
    .name = "es8311-duplex",
    .summary = "ES8311 duplex audio",
    .required_capabilities = SOLAR_OS_BOARD_CAP_I2C |
                             SOLAR_OS_BOARD_CAP_EXPANSION_I2S,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = attach_es8311_duplex,
    .detach = audio_codec_board_detach,
};
