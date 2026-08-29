#include "solar_os_audio_dac_expansion.h"

#include <string.h>

#include "audio_dac_board.h"

static esp_err_t attach_dac(const char *name,
                            const solar_os_expansion_binding_t *bindings,
                            size_t count)
{
    audio_dac_board_config_t config = {
        .dac_pos_pin = -1,
        .dac_neg_pin = -1,
        .amp_pin = -1,
        .amp_active_high = true,
    };
    for (size_t i = 0; i < count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO) {
            if (strcmp(binding->role, "pos") == 0) config.dac_pos_pin = binding->value;
            else if (strcmp(binding->role, "neg") == 0) config.dac_neg_pin = binding->value;
            else if (strcmp(binding->role, "amp") == 0) config.amp_pin = binding->value;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   strcmp(binding->role, "active") == 0) {
            config.amp_active_high = binding->value != 0;
        }
    }
    return audio_dac_board_attach(name, &config);
}

static const int dac_pins[] = {25, 26};
static const int active_levels[] = {0, 1};

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "pos", .value_hint = "gpio25|gpio26", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "pos", .required = true, .allowed_values = dac_pins, .allowed_value_count = 2},
    {.key = "neg", .value_hint = "gpio25|gpio26", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "neg", .allowed_values = dac_pins, .allowed_value_count = 2},
    {.key = "amp", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "amp"},
    {.key = "active", .value_hint = "0|1", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "active", .allowed_values = active_levels, .allowed_value_count = 2},
};

const solar_os_expansion_driver_t solar_os_esp32_dac_expansion_driver = {
    .name = "esp32-dac",
    .summary = "ESP32 internal DAC audio",
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = attach_dac,
    .detach = audio_dac_board_detach,
};
