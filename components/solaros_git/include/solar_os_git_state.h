#pragma once

#include <stddef.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define SOLAR_OS_GIT_STATE_BSS EXT_RAM_BSS_ATTR
#define SOLAR_OS_GIT_STATE_SECTION ".solaros_git_initializers"
#else
#define SOLAR_OS_GIT_STATE_BSS
#define SOLAR_OS_GIT_STATE_SECTION "solaros_git_initializers"
#endif

typedef struct {
    void *destination;
    const void *initial_value;
    size_t size;
} solar_os_git_state_initializer_t;

/* Defaults stay in flash; the writable objects are external BSS. This also
 * works for function-local statics, whose addresses have static lifetime. */
#define SOLAR_OS_GIT_STATE_INITIALIZER(name, initial_value) \
    SOLAR_OS_GIT_STATE_INITIALIZER_EXPAND(name, initial_value, __COUNTER__)
#define SOLAR_OS_GIT_STATE_INITIALIZER_EXPAND(name, initial_value, serial) \
    SOLAR_OS_GIT_STATE_INITIALIZER_IMPL(name, initial_value, serial)
#define SOLAR_OS_GIT_STATE_INITIALIZER_IMPL(name, initial_value, serial) \
    static const solar_os_git_state_initializer_t solar_git_record_##serial \
        __attribute__((used, aligned(__alignof__(solar_os_git_state_initializer_t)), \
                       section(SOLAR_OS_GIT_STATE_SECTION))) = \
        { &(name), &(initial_value), sizeof(name) }

/* Called under the Git service's internal serialization guard, before either
 * library initialization or the options API can access writable globals. */
void solar_os_git_state_init(void);
