#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_task.h"
#include "solar_os_memory.h"

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

static bool solar_os_shutdown_requested(void) { return shutting_down; }

#include "task_admission.inc"

int main(void)
{
    const uint32_t stack = 28U * 1024U;
    solar_os_task_admission_status_t status;
    solar_os_task_get_admission_status(&status);
    assert(status.external_free_bytes == external_free);
    assert(status.external_largest_block_bytes == external_largest);
    assert(status.internal_free_bytes == internal_free);
    assert(status.external_stacks_supported ==
           (SOLAR_OS_FREERTOS_EXTERNAL_MEMORY != 0));

    assert(solar_os_task_can_create(stack, SOLAR_OS_TASK_ROLE_FOREGROUND, true));
    internal_largest = SOLAR_OS_TASK_INTERNAL_OVERHEAD_BYTES;
#if SOLAR_OS_FREERTOS_EXTERNAL_MEMORY
    assert(solar_os_task_can_create(stack, SOLAR_OS_TASK_ROLE_FOREGROUND, true));
    external_largest = stack - 1U;
    assert(!solar_os_task_can_create(stack, SOLAR_OS_TASK_ROLE_FOREGROUND, true));
    external_largest = stack;
    external_free = stack - 1U;
    assert(!solar_os_task_can_create(stack, SOLAR_OS_TASK_ROLE_FOREGROUND, true));
    external_free = stack;
    assert(solar_os_task_can_create(stack, SOLAR_OS_TASK_ROLE_FOREGROUND, true));
#else
    /* Classic/internal-only builds still require the entire internal stack. */
    assert(!solar_os_task_can_create(stack, SOLAR_OS_TASK_ROLE_FOREGROUND, true));
#endif
    assert(!solar_os_task_can_create(stack, SOLAR_OS_TASK_ROLE_FOREGROUND, false));

    internal_free = SOLAR_OS_TASK_INTERNAL_OVERHEAD_BYTES;
    assert(!solar_os_task_can_create(stack, SOLAR_OS_TASK_ROLE_BACKGROUND, true));
    internal_free = SOLAR_OS_TASK_INTERNAL_OVERHEAD_BYTES - 1U;
    assert(!solar_os_task_can_create(stack, SOLAR_OS_TASK_ROLE_FOREGROUND, true));
    internal_free = 64U * 1024U;
    internal_largest = 32U * 1024U;
    assert(solar_os_task_can_create(stack, SOLAR_OS_TASK_ROLE_BACKGROUND, true));

    shutting_down = true;
    assert(!solar_os_task_can_create(stack, SOLAR_OS_TASK_ROLE_FOREGROUND, true));
    assert(!solar_os_task_can_create(0U, SOLAR_OS_TASK_ROLE_FOREGROUND, true));
    assert(!solar_os_task_can_create(stack, SOLAR_OS_TASK_ROLE_COUNT, true));
    puts("task admission managed PSRAM, contiguous stacks and reserves: ok");
    return 0;
}
