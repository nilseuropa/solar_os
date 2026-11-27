#pragma once

#include <stddef.h>

#include "solar_os.h"

typedef struct {
    const char *name;
    const char *summary;
    const solar_os_job_t *job;
} solar_os_job_registry_entry_t;

size_t solar_os_job_registry_count(void);
const solar_os_job_registry_entry_t *solar_os_job_registry_get(size_t index);
