#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Boards without a VBUS switch leave peripheral power to the external supply. */
esp_err_t solar_os_board_usb_set_vbus(bool enabled);
