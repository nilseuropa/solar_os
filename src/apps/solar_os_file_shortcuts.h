#pragma once

#include <stdint.h>

#include "solar_os_input.h"
#include "solar_os_keys.h"

#define SOLAR_OS_FILE_SHORTCUT_SEARCH_USAGE 0x16U
#define SOLAR_OS_FILE_SHORTCUT_ENTER_USAGE 0x28U

typedef enum {
    SOLAR_OS_FILE_SHORTCUT_NONE,
    SOLAR_OS_FILE_SHORTCUT_SEARCH,
    SOLAR_OS_FILE_SHORTCUT_FULLSCREEN,
} solar_os_file_shortcut_t;

static inline solar_os_file_shortcut_t solar_os_file_shortcut_from_key_event(
    const solar_os_input_key_event_t *event)
{
    if (event == NULL || event->action == SOLAR_OS_INPUT_KEY_RELEASE) {
        return SOLAR_OS_FILE_SHORTCUT_NONE;
    }
    if ((event->modifiers & (SOLAR_OS_INPUT_MOD_ALT | SOLAR_OS_INPUT_MOD_CTRL)) != 0U &&
        (event->usage == SOLAR_OS_FILE_SHORTCUT_SEARCH_USAGE ||
         event->key == 's' || event->key == 'S' || event->key == 0x13U)) {
        return SOLAR_OS_FILE_SHORTCUT_SEARCH;
    }
    if ((event->modifiers & SOLAR_OS_INPUT_MOD_ALT) != 0U &&
        (event->usage == SOLAR_OS_FILE_SHORTCUT_ENTER_USAGE ||
         event->key == SOLAR_OS_KEY_ENTER || event->key == '\r')) {
        return SOLAR_OS_FILE_SHORTCUT_FULLSCREEN;
    }
    return SOLAR_OS_FILE_SHORTCUT_NONE;
}
