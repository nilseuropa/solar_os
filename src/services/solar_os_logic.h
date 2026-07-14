#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_LOGIC_MAX_CHANNELS 8U
#define SOLAR_OS_LOGIC_MAX_SAMPLES 32768U
#define SOLAR_OS_LOGIC_MIN_RATE_HZ 10000U
#define SOLAR_OS_LOGIC_MAX_RATE_HZ 2000000U
#define SOLAR_OS_LOGIC_DEFAULT_RATE_HZ 100000U
#define SOLAR_OS_LOGIC_DEFAULT_SAMPLES 4096U

typedef struct {
    uint8_t pins[SOLAR_OS_LOGIC_MAX_CHANNELS];
    uint8_t channel_count;
    uint32_t sample_rate_hz;
    uint32_t sample_count;
} solar_os_logic_config_t;

typedef struct {
    bool capturing;
    bool has_capture;
    solar_os_logic_config_t config;
    uint32_t effective_rate_hz;
    uint32_t generation;
    uint64_t capture_started_us;
    uint64_t capture_duration_us;
    esp_err_t last_error;
} solar_os_logic_status_t;

esp_err_t solar_os_logic_init(void);
esp_err_t solar_os_logic_default_config(solar_os_logic_config_t *config);
esp_err_t solar_os_logic_validate_config(const solar_os_logic_config_t *config);
esp_err_t solar_os_logic_capture(const solar_os_logic_config_t *config);
esp_err_t solar_os_logic_get_status(solar_os_logic_status_t *status);
esp_err_t solar_os_logic_copy_samples(size_t start,
                                      uint8_t *samples,
                                      size_t max_samples,
                                      size_t *copied);
