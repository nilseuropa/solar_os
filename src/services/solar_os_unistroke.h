#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SOLAR_OS_UNISTROKE_POINT_COUNT 64U

typedef struct {
    float x;
    float y;
} solar_os_unistroke_point_t;

typedef struct {
    solar_os_unistroke_point_t points[SOLAR_OS_UNISTROKE_POINT_COUNT];
} solar_os_unistroke_path_t;

typedef struct {
    bool rotate_to_indicative_angle;
    float square_size;
    float angle_range_radians;
    float angle_precision_radians;
    float minimum_score;
} solar_os_unistroke_config_t;

typedef struct {
    solar_os_unistroke_path_t path;
    int value;
    uint32_t flags;
} solar_os_unistroke_template_t;

typedef struct {
    bool matched;
    size_t template_index;
    int value;
    float score;
    float distance;
} solar_os_unistroke_result_t;

typedef enum {
    SOLAR_OS_UNISTROKE_OK = 0,
    SOLAR_OS_UNISTROKE_INVALID_ARGUMENT,
    SOLAR_OS_UNISTROKE_TOO_SHORT,
} solar_os_unistroke_status_t;

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
