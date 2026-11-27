#pragma once

#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_GESTURE_SERVICE "input.gesture"
#define SOLAR_OS_NATIVE_GESTURE_ABI 1U

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

typedef enum {
    SOLAR_OS_GRAFFITI_LETTERS,
    SOLAR_OS_GRAFFITI_NUMBERS,
} solar_os_graffiti_zone_t;

typedef enum {
    SOLAR_OS_GRAFFITI_ACTION_NONE,
    SOLAR_OS_GRAFFITI_ACTION_CHARACTER,
    SOLAR_OS_GRAFFITI_ACTION_SHIFT,
    SOLAR_OS_GRAFFITI_ACTION_SPACE,
    SOLAR_OS_GRAFFITI_ACTION_BACKSPACE,
    SOLAR_OS_GRAFFITI_ACTION_ENTER,
} solar_os_graffiti_action_t;

typedef struct {
    bool matched;
    solar_os_graffiti_action_t action;
    char character;
    float score;
} solar_os_graffiti_result_t;

typedef struct {
    bool shift_pending;
    bool caps_lock;
} solar_os_graffiti_case_state_t;

/* Results and templates live in caller storage. The optional Graffiti methods
 * reuse the built-in gesture alphabet. Its private context needs the size and
 * alignment reported by the table. Allocate it once and initialize it before
 * recognition. It retains no callbacks or additional allocations. */
typedef struct {
    uint32_t abi_version, struct_size;
    solar_os_unistroke_config_t (*defaults)(void);
    solar_os_unistroke_status_t (*prepare)(const solar_os_unistroke_point_t *points,
        size_t point_count, const solar_os_unistroke_config_t *config, solar_os_unistroke_path_t *path);
    solar_os_unistroke_status_t (*recognize)(const solar_os_unistroke_point_t *points,
        size_t point_count, const solar_os_unistroke_template_t *templates, size_t template_count,
        uint32_t allowed_flags, const solar_os_unistroke_config_t *config, solar_os_unistroke_result_t *result);
    size_t (*graffiti_context_size)(void);
    size_t (*graffiti_context_alignment)(void);
    bool (*graffiti_init)(void *storage, size_t storage_size);
    size_t (*graffiti_template_count)(void);
    solar_os_graffiti_zone_t (*graffiti_zone)(int16_t x, uint16_t width);
    bool (*graffiti_recognize)(const void *storage, solar_os_graffiti_zone_t zone,
        const solar_os_unistroke_point_t *points, size_t point_count, solar_os_graffiti_result_t *result);
    bool (*graffiti_apply)(solar_os_graffiti_case_state_t *state, const solar_os_graffiti_result_t *result,
        char *character);
} solar_os_native_gesture_api_v1_t;
