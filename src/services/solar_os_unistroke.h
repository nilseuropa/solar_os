#pragma once

#include "solar_os_native_gesture_abi.h"

solar_os_unistroke_config_t solar_os_unistroke_default_config(void);

solar_os_unistroke_status_t solar_os_unistroke_prepare(
    const solar_os_unistroke_point_t *points,
    size_t point_count,
    const solar_os_unistroke_config_t *config,
    solar_os_unistroke_path_t *path);

solar_os_unistroke_status_t solar_os_unistroke_recognize(
    const solar_os_unistroke_point_t *points,
    size_t point_count,
    const solar_os_unistroke_template_t *templates,
    size_t template_count,
    uint32_t allowed_flags,
    const solar_os_unistroke_config_t *config,
    solar_os_unistroke_result_t *result);
