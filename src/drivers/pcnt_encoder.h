#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/pulse_cnt.h"
#include "esp_err.h"

typedef struct {
    int pin_a;
    int pin_b;
    int mode; /* Required: 1, 2, or 4 counts per complete quadrature cycle. */
    int pull; /* 0: none, 1: up, 2: down. */
    bool invert;
    uint32_t filter_ns; /* Zero disables filtering. */
} pcnt_encoder_config_t;

typedef struct {
    pcnt_unit_handle_t unit;
    pcnt_channel_handle_t channels[2];
    bool enabled;
    bool started;
} pcnt_encoder_t;

/* Caller supplies zeroed storage. PCNT counts in hardware; IDF accumulates
 * 16-bit hardware rollovers into its signed 32-bit count. */
esp_err_t pcnt_encoder_init(pcnt_encoder_t *encoder, const pcnt_encoder_config_t *config);
/* Retryable: resources already released are removed from the handles. */
esp_err_t pcnt_encoder_deinit(pcnt_encoder_t *encoder);
esp_err_t pcnt_encoder_read(pcnt_encoder_t *encoder, int32_t *count);
esp_err_t pcnt_encoder_reset(pcnt_encoder_t *encoder);
