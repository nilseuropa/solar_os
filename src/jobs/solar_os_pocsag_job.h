#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "solar_os.h"
#include "solar_os_pocsag.h"
#include "solar_os_radio.h"

typedef struct {
    bool running;
    char radio[SOLAR_OS_RADIO_NAME_MAX];
    uint32_t frequency_hz;
    uint32_t baud;
    uint32_t ric;
    solar_os_pocsag_format_t format;
    bool inverted;
    uint32_t batches;
    uint32_t messages;
    uint32_t duplicates;
    uint32_t receive_errors;
    uint32_t corrected_codewords;
    uint32_t uncorrectable_codewords;
    int16_t last_rssi_dbm;
    esp_err_t last_error;
} solar_os_pocsag_job_status_t;

extern const solar_os_job_t solar_os_pocsag_job;

void solar_os_pocsag_job_get_status(solar_os_pocsag_job_status_t *status);
