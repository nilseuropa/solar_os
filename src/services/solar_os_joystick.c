#include "solar_os_joystick.h"

#include <stdbool.h>

#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include "solar_os_board_caps.h"
#include "solar_os_log.h"

#if SOLAR_OS_BOARD_HAS_JOYSTICK
#include "solar_os_board.h"

#ifndef SOLAR_OS_BOARD_JOYSTICK_AXES
#error "Board enables JOYSTICK but does not define SOLAR_OS_BOARD_JOYSTICK_AXES."
#endif

#ifndef SOLAR_OS_JOYSTICK_REPEAT_DELAY_MS
#define SOLAR_OS_JOYSTICK_REPEAT_DELAY_MS 350U
#endif
#ifndef SOLAR_OS_JOYSTICK_REPEAT_INTERVAL_MS
#define SOLAR_OS_JOYSTICK_REPEAT_INTERVAL_MS 90U
#endif

#define JOYSTICK_ADC_UNIT_COUNT 2

typedef struct {
    adc_unit_t unit;
    adc_channel_t channel;
    bool configured;
    int direction;
    uint32_t next_repeat_ms;
    uint16_t low_press;
    uint16_t low_release;
    uint16_t high_press;
    uint16_t high_release;
    int last_raw;
    bool last_raw_valid;
    esp_err_t last_read_error;
} joystick_axis_state_t;

static const char *TAG = "solar_os_joystick";
static const solar_os_joystick_axis_def_t joystick_axes[] = SOLAR_OS_BOARD_JOYSTICK_AXES;
static joystick_axis_state_t joystick_states[sizeof(joystick_axes) / sizeof(joystick_axes[0])];
static adc_oneshot_unit_handle_t joystick_units[JOYSTICK_ADC_UNIT_COUNT];
static bool joystick_initialized;

static uint32_t joystick_millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static int joystick_unit_index(adc_unit_t unit)
{
    switch (unit) {
    case ADC_UNIT_1:
        return 0;
    case ADC_UNIT_2:
        return 1;
    default:
        return -1;
    }
}

static esp_err_t joystick_ensure_unit(adc_unit_t unit)
{
    const int index = joystick_unit_index(unit);
    if (index < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (joystick_units[index] != NULL) {
        return ESP_OK;
    }

    adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = unit,
    };
    return adc_oneshot_new_unit(&unit_config, &joystick_units[index]);
}

static esp_err_t joystick_read_raw(size_t axis_index, int *raw)
{
    if (raw == NULL || axis_index >= sizeof(joystick_axes) / sizeof(joystick_axes[0])) {
        return ESP_ERR_INVALID_ARG;
    }

    joystick_axis_state_t *state = &joystick_states[axis_index];
    const int unit_index = joystick_unit_index(state->unit);
    if (!state->configured || unit_index < 0 || joystick_units[unit_index] == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t err = adc_oneshot_read(joystick_units[unit_index], state->channel, raw);
    state->last_read_error = err;
    if (err == ESP_OK) {
        state->last_raw = *raw;
        state->last_raw_valid = true;
    } else {
        state->last_raw_valid = false;
    }
    return err;
}

static int joystick_direction_from_raw(const joystick_axis_state_t *state,
                                       int raw,
                                       int previous_direction)
{
    if (previous_direction < 0 && raw <= state->low_release) {
        return -1;
    }
    if (previous_direction > 0 && raw >= state->high_release) {
        return 1;
    }
    if (raw <= state->low_press) {
        return -1;
    }
    if (raw >= state->high_press) {
        return 1;
    }
    return 0;
}

static uint8_t joystick_key_for_direction(const solar_os_joystick_axis_def_t *axis, int direction)
{
    if (direction < 0) {
        return axis->low_key;
    }
    if (direction > 0) {
        return axis->high_key;
    }
    return 0;
}
#endif

esp_err_t solar_os_joystick_init(void)
{
#if !SOLAR_OS_BOARD_HAS_JOYSTICK
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (joystick_initialized) {
        return ESP_OK;
    }

    for (size_t i = 0; i < sizeof(joystick_axes) / sizeof(joystick_axes[0]); i++) {
        const solar_os_joystick_axis_def_t *axis = &joystick_axes[i];
        adc_unit_t unit;
        adc_channel_t channel;

        esp_err_t ret = adc_oneshot_io_to_channel(axis->pin, &unit, &channel);
        if (ret != ESP_OK) {
            return ret;
        }
        ret = joystick_ensure_unit(unit);
        if (ret != ESP_OK) {
            return ret;
        }

        const int unit_index = joystick_unit_index(unit);
        if (unit_index < 0) {
            return ESP_ERR_INVALID_ARG;
        }

        const adc_oneshot_chan_cfg_t channel_config = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        ret = adc_oneshot_config_channel(joystick_units[unit_index], channel, &channel_config);
        if (ret != ESP_OK) {
            return ret;
        }

        joystick_states[i] = (joystick_axis_state_t) {
            .unit = unit,
            .channel = channel,
            .configured = true,
            .direction = 0,
            .next_repeat_ms = 0,
            .low_press = axis->low_press,
            .low_release = axis->low_release,
            .high_press = axis->high_press,
            .high_release = axis->high_release,
            .last_raw = 0,
            .last_raw_valid = false,
            .last_read_error = ESP_ERR_INVALID_STATE,
        };

        int raw = 0;
        if (joystick_read_raw(i, &raw) == ESP_OK) {
            joystick_states[i].direction = joystick_direction_from_raw(&joystick_states[i], raw, 0);
            if (joystick_states[i].direction != 0) {
                joystick_states[i].next_repeat_ms =
                    joystick_millis() + SOLAR_OS_JOYSTICK_REPEAT_DELAY_MS;
            }
        }
    }

    joystick_initialized = true;
    SOLAR_OS_LOGI(TAG, "%u joystick axes ready", (unsigned)(sizeof(joystick_axes) / sizeof(joystick_axes[0])));
    return ESP_OK;
#endif
}

size_t solar_os_joystick_axis_count(void)
{
#if !SOLAR_OS_BOARD_HAS_JOYSTICK
    return 0;
#else
    return sizeof(joystick_axes) / sizeof(joystick_axes[0]);
#endif
}

bool solar_os_joystick_get_axis_status(size_t index, solar_os_joystick_axis_status_t *status)
{
#if !SOLAR_OS_BOARD_HAS_JOYSTICK
    (void)index;
    (void)status;
    return false;
#else
    if (status == NULL || index >= sizeof(joystick_axes) / sizeof(joystick_axes[0])) {
        return false;
    }

    const solar_os_joystick_axis_def_t *axis = &joystick_axes[index];
    joystick_axis_state_t *state = &joystick_states[index];
    int raw = 0;
    const esp_err_t err = joystick_read_raw(index, &raw);
    const int direction = err == ESP_OK ?
        joystick_direction_from_raw(state, raw, state->direction) :
        state->direction;

    *status = (solar_os_joystick_axis_status_t) {
        .initialized = joystick_initialized && state->configured,
        .pin = axis->pin,
        .name = axis->name,
        .raw = err == ESP_OK ? raw : state->last_raw,
        .raw_valid = err == ESP_OK || state->last_raw_valid,
        .read_error = err,
        .direction = direction,
        .low_key = axis->low_key,
        .high_key = axis->high_key,
        .low_press = state->low_press,
        .low_release = state->low_release,
        .high_press = state->high_press,
        .high_release = state->high_release,
    };
    return true;
#endif
}

esp_err_t solar_os_joystick_calibrate_center(void)
{
#if !SOLAR_OS_BOARD_HAS_JOYSTICK
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!joystick_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t i = 0; i < sizeof(joystick_axes) / sizeof(joystick_axes[0]); i++) {
        joystick_axis_state_t *state = &joystick_states[i];
        int raw = 0;
        const esp_err_t err = joystick_read_raw(i, &raw);
        if (err != ESP_OK) {
            return err;
        }

        const int low_press = raw - 700;
        const int low_release = raw - 350;
        const int high_release = raw + 350;
        const int high_press = raw + 700;
        state->low_press = (uint16_t)(low_press < 0 ? 0 : low_press);
        state->low_release = (uint16_t)(low_release < 0 ? 0 : low_release);
        state->high_release = (uint16_t)(high_release > 4095 ? 4095 : high_release);
        state->high_press = (uint16_t)(high_press > 4095 ? 4095 : high_press);
        state->direction = 0;
        state->next_repeat_ms = 0;
    }
    return ESP_OK;
#endif
}

esp_err_t solar_os_joystick_calibrate_reset(void)
{
#if !SOLAR_OS_BOARD_HAS_JOYSTICK
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!joystick_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t i = 0; i < sizeof(joystick_axes) / sizeof(joystick_axes[0]); i++) {
        const solar_os_joystick_axis_def_t *axis = &joystick_axes[i];
        joystick_axis_state_t *state = &joystick_states[i];
        state->low_press = axis->low_press;
        state->low_release = axis->low_release;
        state->high_press = axis->high_press;
        state->high_release = axis->high_release;
        state->direction = 0;
        state->next_repeat_ms = 0;
    }
    return ESP_OK;
#endif
}

size_t solar_os_joystick_read_chars(char *buffer, size_t buffer_len)
{
#if !SOLAR_OS_BOARD_HAS_JOYSTICK
    (void)buffer;
    (void)buffer_len;
    return 0;
#else
    if (buffer == NULL || buffer_len == 0 || !joystick_initialized) {
        return 0;
    }

    const uint32_t now_ms = joystick_millis();
    size_t count = 0;

    for (size_t i = 0; i < sizeof(joystick_axes) / sizeof(joystick_axes[0]); i++) {
        joystick_axis_state_t *state = &joystick_states[i];
        int raw = 0;
        if (joystick_read_raw(i, &raw) != ESP_OK) {
            continue;
        }

        const int direction = joystick_direction_from_raw(state, raw, state->direction);
        bool emit = false;
        if (direction != state->direction) {
            state->direction = direction;
            state->next_repeat_ms = direction == 0 ?
                0 :
                now_ms + SOLAR_OS_JOYSTICK_REPEAT_DELAY_MS;
            emit = direction != 0;
        } else if (direction != 0 &&
                   (int32_t)(now_ms - state->next_repeat_ms) >= 0) {
            state->next_repeat_ms = now_ms + SOLAR_OS_JOYSTICK_REPEAT_INTERVAL_MS;
            emit = true;
        }

        if (!emit) {
            continue;
        }

        const uint8_t key = joystick_key_for_direction(&joystick_axes[i], direction);
        if (key == 0) {
            continue;
        }

        if (count >= buffer_len) {
            break;
        }
        buffer[count++] = (char)key;
    }

    return count;
#endif
}
