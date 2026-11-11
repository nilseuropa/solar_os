#pragma once
#include "epd_ultrachip.h"

/* Xteink 800x480 glass, 800x600 addressed gates, panel-programmed OTP LUTs.
 * The profile must be selected explicitly; the controller is never probed. */
#define EPD_UC8179_PANEL_XTEINK_800X480 1U
/* Good Display GDEY075T7 (reTerminal E1001): 800x480 gates in order,
 * panel-programmed OTP LUTs. */
#define EPD_UC8179_PANEL_GDEY075T7 2U
esp_err_t epd_uc8179_init(epd_ultrachip_t *display, const epd_ultrachip_config_t *config);
