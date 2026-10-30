#pragma once

#include <stdint.h>

/* Caller-owned budget for synchronous work. Check at bounded work units, in
 * task context outside critical sections and locks needed by other workers.
 * Checkpoints sleep briefly after a time slice, allowing lower-priority and
 * idle tasks to run. They neither feed nor change the watchdog. */
#define SOLAR_OS_WORK_SLICE_US 8000U

typedef struct {
    uint64_t last_yield_us;
} solar_os_work_budget_t;

void solar_os_work_begin(solar_os_work_budget_t *budget);
void solar_os_work_checkpoint(solar_os_work_budget_t *budget);
