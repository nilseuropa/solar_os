#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_expansion.h"

esp_err_t solar_os_gnss_attach(const char *name,
                               const solar_os_expansion_binding_t *bindings,
                               size_t binding_count);
esp_err_t solar_os_gnss_detach(const char *name);
esp_err_t solar_os_gnss_read_raw(uint8_t *buf, size_t len,
                                 uint32_t timeout_ms, size_t *read_len);
esp_err_t solar_os_gnss_write_raw(const uint8_t *buf, size_t len, size_t *written);
