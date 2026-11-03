#pragma once
#include "solar_os_input.h"

/* OS overlay routing precedes application capture. No callbacks execute under
 * the input lock. Consumers drain an ordered, bounded event queue. */
typedef struct {
    char target[SOLAR_OS_INPUT_POINTER_TARGET_MAX];
    solar_os_input_source_t excluded_source, trigger_source, touch_source;
    uint8_t trigger_key, trigger_modifiers;
    uint16_t trigger_physical_key;
    bool navigation, visible, edge_enabled;
    solar_os_input_gesture_direction_t edge_direction;
    uint16_t native_width, native_height, orientation;
    uint8_t scale;
    uint16_t x, y, width, height;
} solar_os_input_route_config_t;

typedef enum {
    SOLAR_OS_INPUT_ROUTE_KEY, SOLAR_OS_INPUT_ROUTE_POINTER, SOLAR_OS_INPUT_ROUTE_GESTURE,
} solar_os_input_route_event_type_t;
typedef struct {
    solar_os_input_route_event_type_t type;
    union {
        solar_os_input_key_event_t key;
        solar_os_input_pointer_sample_t pointer;
        solar_os_input_gesture_event_t gesture;
    } data;
} solar_os_input_route_event_t;

esp_err_t solar_os_input_route_claim(const void *owner, const solar_os_input_route_config_t *config);
esp_err_t solar_os_input_route_update(const void *owner, const solar_os_input_route_config_t *config);
void solar_os_input_route_release(const void *owner);
bool solar_os_input_route_read(const void *owner, solar_os_input_route_event_t *event, bool *reset);
