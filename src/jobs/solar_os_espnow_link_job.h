#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os.h"
#include "solar_os_espnow.h"
#include "solar_os_link.h"

typedef struct {
    bool running;
    bool inbox_enabled;
    bool chat_enabled;
    bool channel_auto;
    uint8_t channel;
    solar_os_espnow_phy_t phy;
    char link[SOLAR_OS_LINK_NAME_MAX];
    uint32_t transmitted;
    uint32_t received;
    uint32_t inbox_published;
    uint32_t chat_errors;
    uint32_t transmit_errors;
    uint32_t receive_errors;
    esp_err_t last_error;
} solar_os_espnow_link_job_status_t;

void solar_os_espnow_link_job_get_status(
    solar_os_espnow_link_job_status_t *status);

extern const solar_os_job_t solar_os_espnow_link_job;
