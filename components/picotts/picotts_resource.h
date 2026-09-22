#pragma once

#include <stddef.h>

#include "picoapi.h"
#include "picorsrc.h"

pico_status_t solar_os_pico_load_resource(pico_System system,
                                           const void *raw,
                                           size_t raw_size,
                                           pico_Resource *resource);
pico_status_t solar_os_pico_unload_resource(pico_System system,
                                             pico_Resource *resource);
