#include "solar_os_qemu_input_codec.h"

#include <limits.h>
#include <string.h>

esp_err_t solar_os_qemu_input_release_all(solar_os_qemu_input_state_t *state)
{
    solar_os_input_source_release_all(state->keyboard);
    memset(state->pressed, 0, sizeof(state->pressed));
    memset(state->keys, 0, sizeof(state->keys));
    state->pointer_published = false;
    for (unsigned i = 0; i < 2; i++) {
        uint8_t *buttons = i == 0 ? &state->touch_buttons : &state->mouse_buttons;
        if (!*buttons) continue;
        solar_os_input_pointer_sample_t release = {.event = {
            .mode = i == 0 ? SOLAR_OS_INPUT_POINTER_ABSOLUTE : SOLAR_OS_INPUT_POINTER_RELATIVE,
            .action = SOLAR_OS_INPUT_POINTER_RELEASE,
            .x = i == 0 ? state->touch_x : 0, .y = i == 0 ? state->touch_y : 0,
        }};
        strcpy(release.event.target, "display0");
        const esp_err_t err = solar_os_input_write_pointer_sample(
            i == 0 ? state->touch : state->mouse, &release);
        if (err != ESP_OK) return err;
        *buttons = 0;
    }
    return ESP_OK;
}

static int16_t clamp_delta(uint32_t value)
{
    const int32_t delta = (int32_t)value;
    return delta < INT16_MIN ? INT16_MIN : delta > INT16_MAX ? INT16_MAX : (int16_t)delta;
}

static esp_err_t key_event(solar_os_qemu_input_state_t *state,
                           const solar_os_qemu_input_event_t *event)
{
    const uint32_t usage = event->a;
    if (usage < 4U || usage >= 256U || event->b > 255U || event->c > 1U) {
        return ESP_ERR_INVALID_ARG;
    }
    const bool down = event->c != 0;
    if (state->pressed[usage] == down) return ESP_OK;
    const bool caps = state->caps_lock ^ (usage == 0x39U && down);
    const uint8_t key = down
        ? solar_os_input_translate_hid_usage((uint16_t)usage, (uint8_t)event->b, caps)
        : state->keys[usage];
    const esp_err_t err = solar_os_input_write_key(state->keyboard,
        (uint16_t)usage, (uint16_t)usage, key, (uint8_t)event->b,
        down ? SOLAR_OS_INPUT_KEY_PRESS : SOLAR_OS_INPUT_KEY_RELEASE);
    if (err == ESP_OK) {
        state->pressed[usage] = down;
        state->keys[usage] = down ? key : 0;
        state->caps_lock = caps;
    }
    return err;
}

static esp_err_t pointer_event(solar_os_qemu_input_state_t *state,
                               const solar_os_qemu_input_event_t *event)
{
    const bool touch = event->type == SOLAR_OS_QEMU_INPUT_TOUCH;
    const solar_os_input_source_t source = touch ? state->touch : state->mouse;
    uint8_t *previous = touch ? &state->touch_buttons : &state->mouse_buttons;
    const uint8_t buttons = event->c & 7U;
    if (!state->pointer_published) {
        solar_os_input_pointer_sample_t sample = {
            .event = {.mode = touch ? SOLAR_OS_INPUT_POINTER_ABSOLUTE : SOLAR_OS_INPUT_POINTER_RELATIVE,
                .buttons = buttons,
                .action = buttons == *previous ? SOLAR_OS_INPUT_POINTER_MOVE :
                    (buttons & ~*previous) ? SOLAR_OS_INPUT_POINTER_PRESS : SOLAR_OS_INPUT_POINTER_RELEASE},
        };
        if (touch) {
            if (event->a > SOLAR_OS_QEMU_INPUT_AXIS_MAX || event->b > SOLAR_OS_QEMU_INPUT_AXIS_MAX ||
                state->width < 1U || state->height < 1U) return ESP_ERR_INVALID_ARG;
            /* Recover the nearest pixel after QEMU's normalized-axis conversion. */
            sample.event.x = (int16_t)((event->a * (state->width - 1U) +
                SOLAR_OS_QEMU_INPUT_AXIS_MAX / 2U) / SOLAR_OS_QEMU_INPUT_AXIS_MAX);
            sample.event.y = (int16_t)((event->b * (state->height - 1U) +
                SOLAR_OS_QEMU_INPUT_AXIS_MAX / 2U) / SOLAR_OS_QEMU_INPUT_AXIS_MAX);
        } else {
            sample.event.delta_x = clamp_delta(event->a);
            sample.event.delta_y = clamp_delta(event->b);
        }
        strcpy(sample.event.target, "display0");
        /* Absolute hover updates are not touch contacts. */
        if (!touch || buttons || *previous) {
            const esp_err_t err = solar_os_input_write_pointer_sample(source, &sample);
            if (err != ESP_OK) return err;
        }
        *previous = buttons;
        if (touch) {
            state->touch_x = sample.event.x;
            state->touch_y = sample.event.y;
        }
        state->pointer_published = true;
    }
    if ((int32_t)event->d != 0) {
        const solar_os_input_axis_event_t wheel = {
            .axis = SOLAR_OS_INPUT_AXIS_Z, .delta = (int32_t)event->d,
        };
        const esp_err_t err = solar_os_input_write_axis(source, &wheel);
        if (err != ESP_OK) return err;
    }
    state->pointer_published = false;
    return ESP_OK;
}

esp_err_t solar_os_qemu_input_process(solar_os_qemu_input_state_t *state,
                                      const solar_os_qemu_input_event_t *event)
{
    if (!state || !event) return ESP_ERR_INVALID_ARG;
    switch (event->type) {
    case SOLAR_OS_QEMU_INPUT_KEY: return key_event(state, event);
    case SOLAR_OS_QEMU_INPUT_TOUCH:
    case SOLAR_OS_QEMU_INPUT_MOUSE: return pointer_event(state, event);
    case SOLAR_OS_QEMU_INPUT_RELEASE_ALL:
        return solar_os_qemu_input_release_all(state);
    default: return ESP_ERR_NOT_SUPPORTED;
    }
}
