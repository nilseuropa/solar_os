#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "solar_os_unistroke.h"

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

/* The context is opaque so applications can place its template cache as needed. */
size_t solar_os_graffiti_context_size(void);
bool solar_os_graffiti_init(void *storage, size_t storage_size);
size_t solar_os_graffiti_template_count(void);

solar_os_graffiti_zone_t solar_os_graffiti_zone_for_start(int16_t x,
                                                           uint16_t width);

bool solar_os_graffiti_recognize(
    const void *context,
    solar_os_graffiti_zone_t zone,
    const solar_os_unistroke_point_t *points,
    size_t point_count,
    solar_os_graffiti_result_t *result);

/* Apply shift/caps state. Returns true when a keyboard character is emitted. */
bool solar_os_graffiti_apply(solar_os_graffiti_case_state_t *state,
                             const solar_os_graffiti_result_t *result,
                             char *character);
