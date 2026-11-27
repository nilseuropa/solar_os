#pragma once

#include <stddef.h>

#include "esp_err.h"

#define SOLAR_OS_CLIPBOARD_MAX_BYTES (256 * 1024)

esp_err_t solar_os_clipboard_set(const char *data, size_t len);
/* Copy an atomic snapshot. The borrowed data() view still expires on mutation. */
esp_err_t solar_os_clipboard_read(size_t offset, void *data, size_t capacity,
    size_t *read_size, size_t *total_size);
const char *solar_os_clipboard_data(size_t *len);
size_t solar_os_clipboard_size(void);
void solar_os_clipboard_clear(void);
