#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_task.h"
#include "solar_os_memory.h"
#include "solar_os_native_runtime_stats.h"

#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_SPIRAM 2U
#define MALLOC_CAP_8BIT 4U

static size_t internal_free = 64U * 1024U;
static size_t internal_largest = 32U * 1024U;
static size_t external_free = 5U * 1024U * 1024U;
static size_t external_largest = 4U * 1024U * 1024U;
static bool shutting_down;

/* The small SDK heap cannot hold Player's 28 KiB stack. Its wrapped
 * allocator can still satisfy that request from the managed PSRAM pool. */
static size_t heap_caps_get_free_size(uint32_t caps)
{
    return caps & MALLOC_CAP_INTERNAL ? internal_free : 4096U;
}

static size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    return caps & MALLOC_CAP_INTERNAL ? internal_largest : 1024U;
}

void solar_os_memory_get_status(solar_os_memory_status_t *status)
{
    memset(status, 0, sizeof(*status));
    status->external.free = external_free;
    status->external.largest_free = external_largest;
}

void solar_os_task_get_status(solar_os_task_status_t *status)
{
    *status = (solar_os_task_status_t){.waiting = 2};
}

void solar_os_memory_get_pressure_stats(solar_os_memory_pressure_stats_t *stats)
{
    *stats = (solar_os_memory_pressure_stats_t){.requests = 11};
}

size_t solar_os_memory_get_owners(solar_os_memory_owner_status_t *owners, size_t capacity)
{
    if (!owners || !capacity) return 0;
    owners[0] = (solar_os_memory_owner_status_t){.owner = "fixture", .bytes = 42};
    return 1;
}

const char *solar_os_memory_class_name(solar_os_memory_class_t memory_class)
{
    assert(memory_class == SOLAR_OS_MEMORY_EXTERNAL_REQUIRED);
    return "external-required";
}

size_t solar_os_memory_reclaim(void) { return 4; }
size_t solar_os_memory_compact(void) { return 10; }

static bool solar_os_shutdown_requested(void) { return shutting_down; }

#include "task_admission.inc"

int main(void)
{
    const solar_os_native_task_admission_api_v1_t *tasks = solar_os_native_runtime_stats_get_service(
        SOLAR_OS_NATIVE_TASK_ADMISSION_SERVICE, SOLAR_OS_NATIVE_TASK_ADMISSION_ABI, sizeof(*tasks));
    const solar_os_native_memory_status_api_v1_t *memory = solar_os_native_runtime_stats_get_service(
        SOLAR_OS_NATIVE_MEMORY_STATUS_SERVICE, SOLAR_OS_NATIVE_MEMORY_STATUS_ABI, sizeof(*memory));
    assert(tasks && memory);
    assert(!solar_os_native_runtime_stats_get_service(SOLAR_OS_NATIVE_TASK_ADMISSION_SERVICE, 2, 8));
    solar_os_task_status_t counters;
    tasks->status(&counters);
    assert(counters.waiting == 2 && !strcmp(tasks->role_name(SOLAR_OS_TASK_ROLE_FOREGROUND), "foreground"));
    solar_os_memory_status_t totals;
    memory->status(&totals);
    assert(totals.external.free == external_free);
    solar_os_memory_pressure_stats_t pressure;
    memory->pressure(&pressure);
    assert(pressure.requests == 11);
    solar_os_memory_owner_status_t owner;
    assert(memory->owners(&owner, 1) == 1 && !strcmp(owner.owner, "fixture") && owner.bytes == 42);
    assert(!strcmp(memory->class_name(SOLAR_OS_MEMORY_EXTERNAL_REQUIRED), "external-required"));
    assert(memory->reclaim() == 4 && memory->compact() == 10);

    const uint32_t stack = 28U * 1024U;
    solar_os_task_admission_status_t status;
    tasks->limits(&status);
    assert(status.external_free_bytes == external_free);
    assert(status.external_largest_block_bytes == external_largest);
    assert(status.internal_free_bytes == internal_free);
    assert(status.external_stacks_supported ==
           (SOLAR_OS_FREERTOS_EXTERNAL_MEMORY != 0));

    assert(tasks->can_create(stack, SOLAR_OS_TASK_ROLE_FOREGROUND, true));
    internal_largest = SOLAR_OS_TASK_INTERNAL_OVERHEAD_BYTES;
#if SOLAR_OS_FREERTOS_EXTERNAL_MEMORY
    assert(tasks->can_create(stack, SOLAR_OS_TASK_ROLE_FOREGROUND, true));
    external_largest = stack - 1U;
    assert(!tasks->can_create(stack, SOLAR_OS_TASK_ROLE_FOREGROUND, true));
    external_largest = stack;
    external_free = stack - 1U;
    assert(!tasks->can_create(stack, SOLAR_OS_TASK_ROLE_FOREGROUND, true));
    external_free = stack;
    assert(tasks->can_create(stack, SOLAR_OS_TASK_ROLE_FOREGROUND, true));
#else
    /* Classic/internal-only builds still require the entire internal stack. */
    assert(!tasks->can_create(stack, SOLAR_OS_TASK_ROLE_FOREGROUND, true));
#endif
    assert(!tasks->can_create(stack, SOLAR_OS_TASK_ROLE_FOREGROUND, false));

    internal_free = SOLAR_OS_TASK_INTERNAL_OVERHEAD_BYTES;
    assert(!tasks->can_create(stack, SOLAR_OS_TASK_ROLE_BACKGROUND, true));
    internal_free = SOLAR_OS_TASK_INTERNAL_OVERHEAD_BYTES - 1U;
    assert(!tasks->can_create(stack, SOLAR_OS_TASK_ROLE_FOREGROUND, true));
    internal_free = 64U * 1024U;
    internal_largest = 32U * 1024U;
    assert(tasks->can_create(stack, SOLAR_OS_TASK_ROLE_BACKGROUND, true));

    shutting_down = true;
    assert(!tasks->can_create(stack, SOLAR_OS_TASK_ROLE_FOREGROUND, true));
    assert(!tasks->can_create(0U, SOLAR_OS_TASK_ROLE_FOREGROUND, true));
    assert(!tasks->can_create(stack, SOLAR_OS_TASK_ROLE_COUNT, true));
    puts("task admission managed PSRAM, contiguous stacks and reserves: ok");
    return 0;
}
