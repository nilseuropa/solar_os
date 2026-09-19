#pragma once

#include "esp_err.h"
#include "solar_os.h"

esp_err_t solar_os_shell_launch_setterm_tui(solar_os_context_t *ctx);
esp_err_t solar_os_shell_launch_network_tui(solar_os_context_t *ctx);
esp_err_t solar_os_shell_launch_wifi_tui(solar_os_context_t *ctx);
esp_err_t solar_os_shell_launch_modem_tui(solar_os_context_t *ctx);
esp_err_t solar_os_shell_launch_radio_tui(solar_os_context_t *ctx);
esp_err_t solar_os_shell_launch_expansion_tui(solar_os_context_t *ctx);
