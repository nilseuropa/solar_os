#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Allocation-free accessors for bounded top-level JSON objects.  These are
 * intended for line-oriented protocols that must not build a cJSON tree.
 */
bool solar_os_json_scan_object_string(const char *json,
                                      const char *key,
                                      char *out,
                                      size_t out_len,
                                      bool *truncated);
bool solar_os_json_scan_object_uint64(const char *json,
                                      const char *key,
                                      uint64_t *out);

esp_err_t solar_os_json_escape_string(const char *source,
                                      char *out,
                                      size_t out_len);

#ifdef __cplusplus
}
#endif
