#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Web schedule editor for the irrigation engine, served by the irrigd
 * job on its own esp_http_server instance (default port 8081). See
 * solar_os_irrig_web.c for the threading contract.
 */
#define SOLAR_OS_IRRIG_WEB_DEFAULT_PORT 8081U

esp_err_t solar_os_irrig_web_start(uint16_t port);
void solar_os_irrig_web_stop(void);
bool solar_os_irrig_web_running(void);

/*
 * Main-loop pump, called by the irrigd job on its 1s tick: applies a
 * pending browser save to the engine and refreshes the config
 * snapshot the page is rendered from.
 */
void solar_os_irrig_web_tick(void);
