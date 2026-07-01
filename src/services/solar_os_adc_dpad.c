#include "solar_os_adc_dpad.h"

#include <stdbool.h>

#include "esp_timer.h"
#include "adc_port.h"
#include "solar_os_board_caps.h"
#include "solar_os_log.h"

#if SOLAR_OS_BOARD_HAS_ADC_DPAD
#include "solar_os_board.h"

#ifndef SOLAR_OS_BOARD_ADC_DPAD_AXES
#error "Board enables ADC_DPAD but does not define SOLAR_OS_BOARD_ADC_DPAD_AXES."
#endif

#ifndef SOLAR_OS_ADC_DPAD_REPEAT_DELAY_MS
#define SOLAR_OS_ADC_DPAD_REPEAT_DELAY_MS 350U
#endif
#ifndef SOLAR_OS_ADC_DPAD_REPEAT_INTERVAL_MS
#define SOLAR_OS_ADC_DPAD_REPEAT_INTERVAL_MS 90U
#endif

#define ADC_DPAD_IDLE_MARGIN 300
#define ADC_DPAD_MIN_IDLE_MAX 100

typedef struct {
    adc_unit_t unit;
    adc_channel_t channel;
    bool configured;
    solar_os_adc_dpad_zone_t zone;
    uint32_t next_repeat_ms;
    uint16_t idle_max;
    uint16_t mid_min;
    uint16_t mid_max;
    uint16_t high_min;
    int last_raw;
    bool last_raw_valid;
    esp_err_t last_read_error;
} adc_dpad_axis_state_t;

static const char *TAG = "solar_os_adc_dpad";
static const solar_os_adc_dpad_axis_def_t dpad_axes[] = SOLAR_OS_BOARD_ADC_DPAD_AXES;
static adc_dpad_axis_state_t dpad_states[sizeof(dpad_axes) / sizeof(dpad_axes[0])];
static bool dpad_initialized;

static uint32_t dpad_millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static esp_err_t dpad_read_raw(size_t axis_index, int *raw)
{
    if (raw == NULL || axis_index >= sizeof(dpad_axes) / sizeof(dpad_axes[0])) {
        return ESP_ERR_INVALID_ARG;
    }

    adc_dpad_axis_state_t *state = &dpad_states[axis_index];
    if (!state->configured) {
        return ESP_ERR_INVALID_STATE;
    }

    adc_port_sample_t sample;
    const esp_err_t err = adc_port_read(dpad_axes[axis_index].pin, &sample);
    state->last_read_error = err;
    if (err == ESP_OK) {
        *raw = sample.raw;
        state->last_raw = sample.raw;
        state->last_raw_valid = true;
    } else {
        state->last_raw_valid = false;
    }
    return err;
}

static solar_os_adc_dpad_zone_t dpad_zone_from_raw(const adc_dpad_axis_state_t *state,
                                                   int raw,
                                                   solar_os_adc_dpad_zone_t previous_zone)
{
    if (raw <= state->idle_max) {
        return SOLAR_OS_ADC_DPAD_ZONE_IDLE;
    }
    if (raw >= state->high_min) {
        return SOLAR_OS_ADC_DPAD_ZONE_HIGH;
    }
    if (raw >= state->mid_min && raw <= state->mid_max) {
        return SOLAR_OS_ADC_DPAD_ZONE_MID;
    }

    if (previous_zone != SOLAR_OS_ADC_DPAD_ZONE_IDLE) {
        return previous_zone;
    }
    return SOLAR_OS_ADC_DPAD_ZONE_IDLE;
}

static uint8_t dpad_key_for_zone(const solar_os_adc_dpad_axis_def_t *axis,
                                 solar_os_adc_dpad_zone_t zone)
{
    if (zone == SOLAR_OS_ADC_DPAD_ZONE_MID) {
        return axis->mid_key;
    }
    if (zone == SOLAR_OS_ADC_DPAD_ZONE_HIGH) {
        return axis->high_key;
    }
    return 0;
}
#endif

esp_err_t solar_os_adc_dpad_init(void)
{
#if !SOLAR_OS_BOARD_HAS_ADC_DPAD
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (dpad_initialized) {
        return ESP_OK;
    }

    for (size_t i = 0; i < sizeof(dpad_axes) / sizeof(dpad_axes[0]); i++) {
        const solar_os_adc_dpad_axis_def_t *axis = &dpad_axes[i];
        adc_unit_t unit;
        adc_channel_t channel;

        if (!adc_port_is_adc_capable(axis->pin, &unit, &channel)) {
            return ESP_ERR_NOT_FOUND;
        }

        esp_err_t ret = adc_port_configure_pin(axis->pin, ADC_ATTEN_DB_12, ADC_BITWIDTH_12);
        if (ret != ESP_OK) {
            return ret;
        }

        dpad_states[i] = (adc_dpad_axis_state_t) {
            .unit = unit,
            .channel = channel,
            .configured = true,
            .zone = SOLAR_OS_ADC_DPAD_ZONE_IDLE,
            .next_repeat_ms = 0,
            .idle_max = axis->idle_max,
            .mid_min = axis->mid_min,
            .mid_max = axis->mid_max,
            .high_min = axis->high_min,
            .last_raw = 0,
            .last_raw_valid = false,
            .last_read_error = ESP_ERR_INVALID_STATE,
        };

        int raw = 0;
        if (dpad_read_raw(i, &raw) == ESP_OK) {
            dpad_states[i].zone = dpad_zone_from_raw(&dpad_states[i],
                                                     raw,
                                                     SOLAR_OS_ADC_DPAD_ZONE_IDLE);
            if (dpad_states[i].zone != SOLAR_OS_ADC_DPAD_ZONE_IDLE) {
                dpad_states[i].next_repeat_ms =
                    dpad_millis() + SOLAR_OS_ADC_DPAD_REPEAT_DELAY_MS;
            }
        }
    }

    dpad_initialized = true;
    SOLAR_OS_LOGI(TAG, "%u ADC D-pad axes ready", (unsigned)(sizeof(dpad_axes) / sizeof(dpad_axes[0])));
    return ESP_OK;
#endif
}

size_t solar_os_adc_dpad_axis_count(void)
{
#if !SOLAR_OS_BOARD_HAS_ADC_DPAD
    return 0;
#else
    return sizeof(dpad_axes) / sizeof(dpad_axes[0]);
#endif
}

bool solar_os_adc_dpad_get_axis_status(size_t index, solar_os_adc_dpad_axis_status_t *status)
{
#if !SOLAR_OS_BOARD_HAS_ADC_DPAD
    (void)index;
    (void)status;
    return false;
#else
    if (status == NULL || index >= sizeof(dpad_axes) / sizeof(dpad_axes[0])) {
        return false;
    }

    const solar_os_adc_dpad_axis_def_t *axis = &dpad_axes[index];
    adc_dpad_axis_state_t *state = &dpad_states[index];
    int raw = 0;
    const esp_err_t err = dpad_read_raw(index, &raw);
    const solar_os_adc_dpad_zone_t zone = err == ESP_OK ?
        dpad_zone_from_raw(state, raw, state->zone) :
        state->zone;

    *status = (solar_os_adc_dpad_axis_status_t) {
        .initialized = dpad_initialized && state->configured,
        .pin = axis->pin,
        .name = axis->name,
        .raw = err == ESP_OK ? raw : state->last_raw,
        .raw_valid = err == ESP_OK || state->last_raw_valid,
        .read_error = err,
        .zone = zone,
        .mid_key = axis->mid_key,
        .high_key = axis->high_key,
        .idle_max = state->idle_max,
        .mid_min = state->mid_min,
        .mid_max = state->mid_max,
        .high_min = state->high_min,
    };
    return true;
#endif
}

esp_err_t solar_os_adc_dpad_calibrate_idle(void)
{
#if !SOLAR_OS_BOARD_HAS_ADC_DPAD
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!dpad_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t i = 0; i < sizeof(dpad_axes) / sizeof(dpad_axes[0]); i++) {
        adc_dpad_axis_state_t *state = &dpad_states[i];
        int sum = 0;
        int samples = 0;
        for (size_t n = 0; n < 16; n++) {
            int raw = 0;
            if (dpad_read_raw(i, &raw) == ESP_OK) {
                sum += raw;
                samples++;
            }
        }
        if (samples == 0) {
            return state->last_read_error;
        }

        const int average = sum / samples;
        int idle_max = average + ADC_DPAD_IDLE_MARGIN;
        const int highest_idle_max = (int)state->mid_min - 100;
        if (idle_max < ADC_DPAD_MIN_IDLE_MAX) {
            idle_max = ADC_DPAD_MIN_IDLE_MAX;
        }
        if (idle_max > highest_idle_max) {
            idle_max = highest_idle_max;
        }
        if (idle_max < 0) {
            idle_max = 0;
        }
        state->idle_max = (uint16_t)idle_max;
        state->zone = SOLAR_OS_ADC_DPAD_ZONE_IDLE;
        state->next_repeat_ms = 0;
    }
    return ESP_OK;
#endif
}

esp_err_t solar_os_adc_dpad_calibrate_reset(void)
{
#if !SOLAR_OS_BOARD_HAS_ADC_DPAD
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!dpad_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    for (size_t i = 0; i < sizeof(dpad_axes) / sizeof(dpad_axes[0]); i++) {
        const solar_os_adc_dpad_axis_def_t *axis = &dpad_axes[i];
        adc_dpad_axis_state_t *state = &dpad_states[i];
        state->idle_max = axis->idle_max;
        state->mid_min = axis->mid_min;
        state->mid_max = axis->mid_max;
        state->high_min = axis->high_min;
        state->zone = SOLAR_OS_ADC_DPAD_ZONE_IDLE;
        state->next_repeat_ms = 0;
    }
    return ESP_OK;
#endif
}

size_t solar_os_adc_dpad_read_chars(char *buffer, size_t buffer_len)
{
#if !SOLAR_OS_BOARD_HAS_ADC_DPAD
    (void)buffer;
    (void)buffer_len;
    return 0;
#else
    if (buffer == NULL || buffer_len == 0 || !dpad_initialized) {
        return 0;
    }

    const uint32_t now_ms = dpad_millis();
    size_t count = 0;

    for (size_t i = 0; i < sizeof(dpad_axes) / sizeof(dpad_axes[0]); i++) {
        adc_dpad_axis_state_t *state = &dpad_states[i];
        int raw = 0;
        if (dpad_read_raw(i, &raw) != ESP_OK) {
            continue;
        }

        const solar_os_adc_dpad_zone_t zone = dpad_zone_from_raw(state, raw, state->zone);
        bool emit = false;
        if (zone != state->zone) {
            state->zone = zone;
            state->next_repeat_ms = zone == SOLAR_OS_ADC_DPAD_ZONE_IDLE ?
                0 :
                now_ms + SOLAR_OS_ADC_DPAD_REPEAT_DELAY_MS;
            emit = zone != SOLAR_OS_ADC_DPAD_ZONE_IDLE;
        } else if (zone != SOLAR_OS_ADC_DPAD_ZONE_IDLE &&
                   (int32_t)(now_ms - state->next_repeat_ms) >= 0) {
            state->next_repeat_ms = now_ms + SOLAR_OS_ADC_DPAD_REPEAT_INTERVAL_MS;
            emit = true;
        }

        if (!emit) {
            continue;
        }

        const uint8_t key = dpad_key_for_zone(&dpad_axes[i], zone);
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
