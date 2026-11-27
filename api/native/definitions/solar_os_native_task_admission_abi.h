#pragma once

#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_TASK_ADMISSION_SERVICE "task.admission"
#define SOLAR_OS_NATIVE_TASK_ADMISSION_ABI 1U

#define SOLAR_OS_TASK_NAME_MAX 24U

typedef enum {
    SOLAR_OS_TASK_ROLE_SYSTEM = 0,
    SOLAR_OS_TASK_ROLE_FOREGROUND,
    SOLAR_OS_TASK_ROLE_BACKGROUND,
    SOLAR_OS_TASK_ROLE_COUNT,
} solar_os_task_role_t;

typedef struct {
    uint32_t requests;
    uint32_t successes;
    uint32_t denied;
    uint32_t failures;
    uint64_t requested_stack_bytes;
} solar_os_task_role_stats_t;

typedef struct {
    solar_os_task_role_stats_t roles[SOLAR_OS_TASK_ROLE_COUNT];
    uint32_t waiting;
    uint32_t wait_successes;
    uint32_t wait_cancellations;
    bool last_failure_valid;
    bool last_failure_denied;
    bool last_failure_external_stack;
    solar_os_task_role_t last_failure_role;
    uint32_t last_failure_stack_bytes;
    uint32_t last_failure_internal_free_bytes;
    uint32_t last_failure_internal_largest_block_bytes;
    uint32_t last_failure_external_free_bytes;
    uint32_t last_failure_external_largest_block_bytes;
    char last_failure_name[SOLAR_OS_TASK_NAME_MAX];
} solar_os_task_status_t;

typedef struct {
    uint32_t internal_free_bytes;
    uint32_t internal_largest_block_bytes;
    uint32_t external_free_bytes;
    uint32_t external_largest_block_bytes;
    uint32_t background_internal_reserve_bytes;
    uint32_t task_internal_overhead_bytes;
    bool external_stacks_supported;
} solar_os_task_admission_status_t;

/* Admission is advisory. It applies the same memory and shutdown checks as
 * the Core scheduler. Use external_stacks_supported to inspect stack
 * placement support on the current board. */
typedef struct {
    uint32_t abi_version, struct_size;
    void (*status)(solar_os_task_status_t *status);
    void (*limits)(solar_os_task_admission_status_t *limits);
    bool (*can_create)(uint32_t stack_bytes, solar_os_task_role_t role, bool external_stack);
    const char *(*role_name)(solar_os_task_role_t role);
} solar_os_native_task_admission_api_v1_t;
