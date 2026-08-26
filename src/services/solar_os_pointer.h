#pragma once

#include "esp_err.h"

esp_err_t solar_os_pointer_init(void);
void solar_os_pointer_poll(void);
void solar_os_pointer_deinit(void);
