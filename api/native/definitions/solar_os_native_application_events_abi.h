#pragma once
#include "solar_os_input_types_v1.h"

/* The host owns this context. Its layout, queues and session state are private.
 * An application may use an opaque reference only during its owning callback;
 * application service methods query the calling task's active scope. */
typedef struct solar_os_context solar_os_application_context_t;

typedef enum {
    SOLAR_OS_EVENT_CHAR = 0,
    SOLAR_OS_EVENT_KEY = 1,
    SOLAR_OS_EVENT_POINTER = 2,
    SOLAR_OS_EVENT_AXIS = 3,
    SOLAR_OS_EVENT_GESTURE = 4,
    SOLAR_OS_EVENT_TICK = 5,
    SOLAR_OS_EVENT_RESUME = 6,
    SOLAR_OS_EVENT_TEXT = 7,
} solar_os_application_event_type_v1_t;

/* TEXT holds one NUL-terminated UTF-8 character; CHAR holds a normalized key
 * byte. Structured input retains the canonical input records and their reset
 * semantics. TICK is the wrapping millisecond clock. Event storage is borrowed
 * for one callback; copy values if they must survive it. */
#define SOLAR_OS_EVENT_TEXT_MAX 5U
typedef struct solar_os_application_event_v1 {
    solar_os_application_event_type_v1_t type;
    union {
        char ch;
        char text[SOLAR_OS_EVENT_TEXT_MAX];
        solar_os_input_key_event_t key;
        solar_os_input_pointer_event_t pointer;
        solar_os_input_axis_event_t axis;
        solar_os_input_gesture_event_t gesture;
        uint32_t tick_ms;
    } data;
} solar_os_application_event_v1_t;
