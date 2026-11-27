/* Private input copied directly into the on-device compiler's source buffer. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int bytes;
    bool (*read)(uint8_t *buffer, size_t bytes);
} solar_os_cc_runtime_source_t;

extern const solar_os_cc_runtime_source_t solar_os_cc_standard_source;
