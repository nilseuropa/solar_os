#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "solar_os_media_sample.h"
#include "solar_os_native_processor_abi.h"

/* Native jobs outlive clients. Up to four configurations/latest-result slots
 * remain until explicit destroy. Stop joins and releases sources/processors;
 * retained models remain resident. Handles never recycle during a boot. */
esp_err_t solar_os_pipeline_start(const solar_os_pipeline_config_t *config, uint32_t *id);
esp_err_t solar_os_pipeline_list(uint32_t *ids, size_t capacity, size_t *count);
esp_err_t solar_os_pipeline_status(uint32_t id, solar_os_pipeline_status_t *status);
/* Non-destructive snapshot. NULL means no new result after `after`. The caller
 * owns the copied JSON (solar_os_memory_free). Sequence and frame timing are
 * included in each result to keep them consistent with that frame. */
esp_err_t solar_os_pipeline_result(uint32_t id, uint64_t after, char **json);
/* Atomic result + matching encoded image snapshot. Either output is optional;
 * both are NULL when no newer result exists. Free JSON and release the sample; that
 * reference survives replacement, stop, restart, and pipeline destruction. */
esp_err_t solar_os_pipeline_result_sample(uint32_t id, uint64_t after, char **json,
    solar_os_media_sample_t **sample);
esp_err_t solar_os_pipeline_stop(uint32_t id);
esp_err_t solar_os_pipeline_destroy(uint32_t id);
