#pragma once

#include <stddef.h>

#include "esp_err.h"

esp_err_t solar_os_cardkb_init(void);
size_t solar_os_cardkb_read_chars(char *buffer, size_t buffer_len);
