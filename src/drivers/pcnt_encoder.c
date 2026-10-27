#include "pcnt_encoder.h"

#include "driver/gpio.h"

static pcnt_channel_edge_action_t action(bool increase, bool invert)
{
    return increase != invert ? PCNT_CHANNEL_EDGE_ACTION_INCREASE : PCNT_CHANNEL_EDGE_ACTION_DECREASE;
}

esp_err_t pcnt_encoder_deinit(pcnt_encoder_t *encoder)
{
    if (encoder == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t ret;
    if (encoder->started) {
        ret = pcnt_unit_stop(encoder->unit);
        if (ret != ESP_OK) return ret;
        encoder->started = false;
    }
    if (encoder->enabled) {
        ret = pcnt_unit_disable(encoder->unit);
        if (ret != ESP_OK) return ret;
        encoder->enabled = false;
    }
    for (unsigned i = 0; i < 2; i++) if (encoder->channels[i] != NULL) {
        ret = pcnt_del_channel(encoder->channels[i]);
        if (ret != ESP_OK) return ret;
        encoder->channels[i] = NULL;
    }
    if (encoder->unit != NULL) {
        ret = pcnt_del_unit(encoder->unit);
        if (ret != ESP_OK) return ret;
        encoder->unit = NULL;
    }
    return ESP_OK;
}

esp_err_t pcnt_encoder_init(pcnt_encoder_t *encoder, const pcnt_encoder_config_t *config)
{
    if (encoder == NULL || config == NULL || encoder->unit != NULL ||
        !GPIO_IS_VALID_GPIO(config->pin_a) || !GPIO_IS_VALID_GPIO(config->pin_b) ||
        config->pin_a == config->pin_b || (config->mode != 1 && config->mode != 2 && config->mode != 4) ||
        config->pull < 0 || config->pull > 2) return ESP_ERR_INVALID_ARG;
    const pcnt_unit_config_t unit_config = {
        .low_limit = -32767, .high_limit = 32767, .flags.accum_count = true,
    };
    esp_err_t ret = pcnt_new_unit(&unit_config, &encoder->unit);
    if (ret != ESP_OK) return ret;
    const pcnt_glitch_filter_config_t filter = {.max_glitch_ns = config->filter_ns};
    ret = pcnt_unit_set_glitch_filter(encoder->unit, config->filter_ns ? &filter : NULL);
    if (ret != ESP_OK) goto fail;
    /* PCNT enables the input path but does not disable a previous GPIO output. */
    ret = gpio_set_direction(config->pin_a, GPIO_MODE_INPUT);
    if (ret != ESP_OK) goto fail;
    ret = gpio_set_direction(config->pin_b, GPIO_MODE_INPUT);
    if (ret != ESP_OK) goto fail;
    const unsigned channel_count = config->mode == 4 ? 2U : 1U;
    for (unsigned i = 0; i < channel_count; i++) {
        const pcnt_chan_config_t channel = {
            .edge_gpio_num = i == 0 ? config->pin_a : config->pin_b,
            .level_gpio_num = i == 0 ? config->pin_b : config->pin_a,
        };
        ret = pcnt_new_channel(encoder->unit, &channel, &encoder->channels[i]);
        if (ret != ESP_OK) goto fail;
        const pcnt_channel_edge_action_t rising = action(i != 0, config->invert);
        const pcnt_channel_edge_action_t falling = config->mode == 1 ?
            PCNT_CHANNEL_EDGE_ACTION_HOLD : action(i == 0, config->invert);
        ret = pcnt_channel_set_edge_action(encoder->channels[i], rising, falling);
        if (ret != ESP_OK) goto fail;
        ret = pcnt_channel_set_level_action(encoder->channels[i],
            PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
        if (ret != ESP_OK) goto fail;
    }
    const gpio_pull_mode_t pull = config->pull == 1 ? GPIO_PULLUP_ONLY :
        config->pull == 2 ? GPIO_PULLDOWN_ONLY : GPIO_FLOATING;
    ret = gpio_set_pull_mode(config->pin_a, pull);
    if (ret != ESP_OK) goto fail;
    ret = gpio_set_pull_mode(config->pin_b, pull);
    if (ret != ESP_OK) goto fail;
    ret = pcnt_unit_add_watch_point(encoder->unit, -32767);
    if (ret != ESP_OK) goto fail;
    ret = pcnt_unit_add_watch_point(encoder->unit, 32767);
    if (ret != ESP_OK) goto fail;
    ret = pcnt_unit_enable(encoder->unit);
    if (ret != ESP_OK) goto fail;
    encoder->enabled = true;
    ret = pcnt_unit_clear_count(encoder->unit);
    if (ret != ESP_OK) goto fail;
    ret = pcnt_unit_start(encoder->unit);
    if (ret != ESP_OK) goto fail;
    encoder->started = true;
    return ESP_OK;
fail:
    (void)pcnt_encoder_deinit(encoder);
    return ret;
}

esp_err_t pcnt_encoder_read(pcnt_encoder_t *encoder, int32_t *count)
{
    if (encoder == NULL || count == NULL) return ESP_ERR_INVALID_ARG;
    if (!encoder->started) return ESP_ERR_INVALID_STATE;
    int value;
    const esp_err_t ret = pcnt_unit_get_count(encoder->unit, &value);
    if (ret == ESP_OK) *count = value;
    return ret;
}

esp_err_t pcnt_encoder_reset(pcnt_encoder_t *encoder)
{
    if (encoder == NULL) return ESP_ERR_INVALID_ARG;
    if (!encoder->started) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = pcnt_unit_stop(encoder->unit);
    if (ret != ESP_OK) return ret;
    encoder->started = false;
    ret = pcnt_unit_clear_count(encoder->unit);
    const esp_err_t restart = pcnt_unit_start(encoder->unit);
    if (restart == ESP_OK) encoder->started = true;
    return ret != ESP_OK ? ret : restart;
}
