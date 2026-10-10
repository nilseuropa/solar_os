#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Validate an unencrypted single-model envelope and its FlatBuffer before any
 * ESP-DL access. Structural validation does not replace operator semantics. */
esp_err_t solar_os_espdl_validate(const uint8_t *data, size_t size,
    size_t *offset, size_t *payload_size, size_t *layers);
#ifdef __cplusplus
}
#endif
