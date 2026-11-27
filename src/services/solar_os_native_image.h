#pragma once
/* Optional ELF backend. Core module state retains only this opaque image. */
#include <stddef.h>
#include "esp_err.h"
esp_err_t solar_os_native_image_load(const char *path, void **image, char *detail, size_t detail_len);
int solar_os_native_image_request(void *image, int argc, char **argv);
void solar_os_native_image_close(void *image);
