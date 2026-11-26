#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define SOLAR_OS_GATEWAY_SYNC_JOB_NAME "gateway-sync"
#define SOLAR_OS_GATEWAY_SYNC_WORKER_STACK 6144U

/* One supervisor owns the configured Gateway instances. Stop joins the
 * child transports and can be retried after a timeout. */
esp_err_t solar_os_gateway_sync_start(void);
esp_err_t solar_os_gateway_sync_stop(void);
bool solar_os_gateway_sync_running(void);
