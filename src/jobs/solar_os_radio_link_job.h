#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os.h"
#include "solar_os_link.h"
#include "solar_os_radio.h"

typedef struct {
    bool running;
    bool inbox_enabled;
    bool chat_enabled;
    bool repeater_enabled;
    char link[SOLAR_OS_LINK_NAME_MAX];
    char radio[SOLAR_OS_RADIO_NAME_MAX];
    char profile[SOLAR_OS_RADIO_PROFILE_NAME_MAX];
    uint32_t transmitted;
    uint32_t received;
    uint32_t inbox_published;
    uint32_t chat_errors;
    uint32_t transmit_errors;
    uint32_t receive_errors;
    size_t repeater_queued;
    uint32_t repeated;
    uint32_t repeater_suppressed;
    uint32_t repeater_queue_drops;
    uint32_t repeater_invalid_frames;
    esp_err_t last_error;
} solar_os_radio_link_job_status_t;

void solar_os_radio_link_job_get_status(solar_os_radio_link_job_status_t *status);

extern const solar_os_job_t solar_os_radio_link_job;
