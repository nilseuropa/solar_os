#include "solar_os_ft6336.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "ft6336.h"
#include "solar_os_board.h"
#include "solar_os_display.h"
#include "solar_os_input.h"

typedef struct {
    bool active;
    bool pressed;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    int reset_pin;
    int irq_pin;
    uint8_t pointer_id;
    int16_t x;
    int16_t y;
    solar_os_input_source_t input_source;
} solar_os_ft6336_device_t;

static const char *TAG = "ft6336";
static solar_os_ft6336_device_t touch;

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                solar_os_ft6336_device_t *device)
{
    bool have_i2c = false;
    bool have_address = false;
    bool have_reset = false;
    bool have_irq = false;

    if (bindings == NULL || device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    device->reset_pin = -1;
    device->irq_pin = -1;

    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        switch (binding->kind) {
        case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
            if (have_i2c) {
                return ESP_ERR_INVALID_ARG;
            }
            strlcpy(device->i2c_bus,
                    binding->target,
                    sizeof(device->i2c_bus));
            have_i2c = true;
            break;
        case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
            if (have_address || binding->value != FT6336_ADDRESS) {
                return ESP_ERR_INVALID_ARG;
            }
            device->address = (uint8_t)binding->value;
            have_address = true;
            break;
        case SOLAR_OS_EXPANSION_BINDING_GPIO:
            if (strcmp(binding->role, "reset") == 0 && !have_reset) {
                device->reset_pin = binding->value;
                have_reset = true;
            } else if (strcmp(binding->role, "irq") == 0 && !have_irq) {
                device->irq_pin = binding->value;
                have_irq = true;
            } else {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        default:
            return ESP_ERR_INVALID_ARG;
        }
    }

    return have_i2c && have_address && have_reset && have_irq
        ? ESP_OK
        : ESP_ERR_INVALID_ARG;
}

static void clear_device(void)
{
    if (touch.input_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        solar_os_input_source_close(touch.input_source);
    }
    ft6336_deinit();
    memset(&touch, 0, sizeof(touch));
}

esp_err_t solar_os_ft6336_attach(const char *name,
                                 const solar_os_expansion_binding_t *bindings,
                                 size_t binding_count)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (touch.active) {
        return ESP_ERR_INVALID_STATE;
    }

    solar_os_ft6336_device_t candidate = {0};
    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count, &candidate),
                        TAG,
                        "invalid bindings");
    ESP_RETURN_ON_ERROR(ft6336_init(candidate.i2c_bus,
                                    candidate.address,
                                    candidate.reset_pin,
                                    candidate.irq_pin),
                        TAG,
                        "controller init failed");

    strlcpy(candidate.name, name, sizeof(candidate.name));
    esp_err_t err = solar_os_input_touch_source_open(candidate.name,
                                                     &candidate.input_source);
    if (err != ESP_OK) {
        ft6336_deinit();
        return err;
    }
    candidate.active = true;
    touch = candidate;
    return ESP_OK;
}

esp_err_t solar_os_ft6336_detach(const char *name)
{
    if (!touch.active || name == NULL || strcmp(touch.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    clear_device();
    return ESP_OK;
}

void solar_os_ft6336_poll(void)
{
    if (!touch.active) {
        return;
    }

    ft6336_sample_t sample;
    if (ft6336_read(&sample) != ESP_OK) {
        return;
    }

    uint16_t sample_x = 0;
    uint16_t sample_y = 0;
    if (sample.touched) {
        if (sample.x >= SOLAR_OS_BOARD_DISPLAY_NATIVE_WIDTH ||
            sample.y >= SOLAR_OS_BOARD_DISPLAY_NATIVE_HEIGHT) {
            return;
        }
        /* Controller coordinates are portrait; the board display is rotation 1. */
        sample_x = sample.y;
        sample_y = (SOLAR_OS_BOARD_DISPLAY_NATIVE_WIDTH - 1U) - sample.x;
    }

    solar_os_input_pointer_action_t action;
    if (sample.touched && !touch.pressed) {
        action = SOLAR_OS_INPUT_POINTER_PRESS;
    } else if (!sample.touched && touch.pressed) {
        action = SOLAR_OS_INPUT_POINTER_RELEASE;
    } else if (sample.touched &&
               (sample_x != (uint16_t)touch.x ||
                sample_y != (uint16_t)touch.y)) {
        action = SOLAR_OS_INPUT_POINTER_MOVE;
    } else {
        return;
    }

    const int16_t next_x = sample.touched ? (int16_t)sample_x : touch.x;
    const int16_t next_y = sample.touched ? (int16_t)sample_y : touch.y;
    solar_os_input_pointer_event_t event = {
        .pointer_id = sample.touched ? sample.id : touch.pointer_id,
        .buttons = sample.touched ? SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY : 0,
        .mode = SOLAR_OS_INPUT_POINTER_ABSOLUTE,
        .action = action,
        .x = next_x,
        .y = next_y,
        .delta_x = (int16_t)(next_x - touch.x),
        .delta_y = (int16_t)(next_y - touch.y),
    };
    strlcpy(event.target,
            SOLAR_OS_DISPLAY_PRIMARY_TARGET,
            sizeof(event.target));
    if (solar_os_input_write_pointer(touch.input_source, &event) == ESP_OK) {
        touch.pressed = sample.touched;
        touch.pointer_id = event.pointer_id;
        touch.x = next_x;
        touch.y = next_y;
    }
}
