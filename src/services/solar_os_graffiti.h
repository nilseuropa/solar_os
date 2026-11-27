#pragma once

#include "solar_os_unistroke.h"

/* The context is opaque so applications can place its template cache as needed. */
size_t solar_os_graffiti_context_size(void);
size_t solar_os_graffiti_context_alignment(void);
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
