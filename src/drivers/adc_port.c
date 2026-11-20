#include "adc_port.h"

#include <stddef.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define ADC_PORT_UNIT_COUNT 2
#define ADC_PORT_CHANNEL_COUNT 11

typedef enum {
    ADC_PORT_CALI_NONE,
    ADC_PORT_CALI_CURVE_FITTING,
    ADC_PORT_CALI_LINE_FITTING,
} adc_port_cali_scheme_t;

typedef struct {
    bool configured;
    adc_atten_t atten;
    adc_bitwidth_t bitwidth;
    adc_cali_handle_t cali_handle;
    adc_port_cali_scheme_t cali_scheme;
    bool calibrated;
} adc_port_channel_state_t;

static const char *TAG = "adc_port";

static adc_oneshot_unit_handle_t adc_units[ADC_PORT_UNIT_COUNT];
static adc_port_channel_state_t adc_channels[ADC_PORT_UNIT_COUNT][ADC_PORT_CHANNEL_COUNT];
static SemaphoreHandle_t adc_mutex;

static int adc_unit_index(adc_unit_t unit)
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

static esp_err_t adc_port_ensure_init(void)
{
    if (adc_mutex != NULL) {
        return ESP_OK;
    }

    adc_mutex = xSemaphoreCreateMutex();
    if (adc_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t adc_port_ensure_unit_locked(adc_unit_t unit)
{
    const int index = adc_unit_index(unit);
    if (index < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (adc_units[index] != NULL) {
        return ESP_OK;
    }

    adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = unit,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_config, &adc_units[index]),
                        TAG,
                        "new ADC unit failed");
    return ESP_OK;
}

static esp_err_t adc_port_init_calibration_locked(adc_unit_t unit,
                                                  adc_channel_t channel,
                                                  adc_atten_t atten,
                                                  adc_bitwidth_t bitwidth,
                                                  adc_port_channel_state_t *state)
{
    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;

    state->cali_handle = NULL;
    state->cali_scheme = ADC_PORT_CALI_NONE;
    state->calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t curve_config = {
        .unit_id = unit,
        .chan = channel,
        .atten = atten,
        .bitwidth = bitwidth,
    };
    ret = adc_cali_create_scheme_curve_fitting(&curve_config, &state->cali_handle);
    if (ret == ESP_OK) {
        state->cali_scheme = ADC_PORT_CALI_CURVE_FITTING;
        state->calibrated = true;
        return ESP_OK;
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t line_config = {
        .unit_id = unit,
        .atten = atten,
        .bitwidth = bitwidth,
    };
    ret = adc_cali_create_scheme_line_fitting(&line_config, &state->cali_handle);
    if (ret == ESP_OK) {
        state->cali_scheme = ADC_PORT_CALI_LINE_FITTING;
        state->calibrated = true;
        return ESP_OK;
    }
#endif

    ESP_LOGW(TAG,
             "ADC calibration unavailable: unit=%d channel=%d: %s",
             unit,
             channel,
             esp_err_to_name(ret));
    return ESP_OK;
}

esp_err_t adc_port_init(void)
{
    return adc_port_ensure_init();
}

bool adc_port_is_adc_capable(gpio_num_t pin, adc_unit_t *unit, adc_channel_t *channel)
{
    adc_unit_t mapped_unit;
    adc_channel_t mapped_channel;

    const esp_err_t err = adc_oneshot_io_to_channel(pin, &mapped_unit, &mapped_channel);
    if (err != ESP_OK) {
        return false;
    }

    if (unit != NULL) {
        *unit = mapped_unit;
    }
    if (channel != NULL) {
        *channel = mapped_channel;
    }
    return true;
}

esp_err_t adc_port_configure_pin(gpio_num_t pin, adc_atten_t atten, adc_bitwidth_t bitwidth)
{
    adc_unit_t unit;
    adc_channel_t channel;

    if (!adc_port_is_adc_capable(pin, &unit, &channel)) {
        return ESP_ERR_NOT_FOUND;
    }

    const int unit_index = adc_unit_index(unit);
    if (unit_index < 0 || channel < 0 || channel >= ADC_PORT_CHANNEL_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(adc_port_ensure_init(), TAG, "ADC port init failed");
    xSemaphoreTake(adc_mutex, portMAX_DELAY);

    esp_err_t ret = adc_port_ensure_unit_locked(unit);
    if (ret == ESP_OK) {
        adc_port_channel_state_t *state = &adc_channels[unit_index][channel];
        adc_oneshot_chan_cfg_t channel_config = {
            .atten = atten,
            .bitwidth = bitwidth,
        };
        ret = adc_oneshot_config_channel(adc_units[unit_index], channel, &channel_config);
        if (ret == ESP_OK) {
            /* Another owner (USB, GPIO, or a bus) may have changed the pin mux
             * since the previous sample. Reapply I/O, but retain calibration. */
            if (state->configured && state->atten == atten && state->bitwidth == bitwidth) {
                xSemaphoreGive(adc_mutex);
                return ESP_OK;
            }
            state->configured = true;
            state->atten = atten;
            state->bitwidth = bitwidth;
            ret = adc_port_init_calibration_locked(unit, channel, atten, bitwidth, state);
        }
    }

    xSemaphoreGive(adc_mutex);
    return ret;
}

esp_err_t adc_port_read(gpio_num_t pin, adc_port_sample_t *sample)
{
    adc_unit_t unit;
    adc_channel_t channel;

    if (sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!adc_port_is_adc_capable(pin, &unit, &channel)) {
        return ESP_ERR_NOT_FOUND;
    }

    const int unit_index = adc_unit_index(unit);
    if (unit_index < 0 || channel < 0 || channel >= ADC_PORT_CHANNEL_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(adc_port_configure_pin(pin, ADC_ATTEN_DB_12, ADC_BITWIDTH_12),
                        TAG,
                        "ADC pin config failed");

    xSemaphoreTake(adc_mutex, portMAX_DELAY);

    int raw = 0;
    esp_err_t ret = adc_oneshot_read(adc_units[unit_index], channel, &raw);
    int voltage_mv = 0;
    adc_port_channel_state_t *state = &adc_channels[unit_index][channel];
    bool calibrated = state->calibrated;
    if (ret == ESP_OK && calibrated) {
        ret = adc_cali_raw_to_voltage(state->cali_handle, raw, &voltage_mv);
        if (ret != ESP_OK) {
            calibrated = false;
            voltage_mv = 0;
        }
    }

    xSemaphoreGive(adc_mutex);

    if (ret != ESP_OK) {
        return ret;
    }

    *sample = (adc_port_sample_t) {
        .pin = pin,
        .raw = raw,
        .voltage_mv = (uint16_t)(voltage_mv < 0 ? 0 : voltage_mv),
        .unit = unit,
        .channel = channel,
        .calibrated = calibrated,
    };
    return ESP_OK;
}
