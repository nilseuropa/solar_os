#pragma once

#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_JOBS_SERVICE "jobs"
#define SOLAR_OS_NATIVE_JOBS_ABI 1U

#define SOLAR_OS_JOB_NAME_MAX 48U
#define SOLAR_OS_JOB_SUMMARY_MAX 96U
#define SOLAR_OS_JOB_OWNER_MAX 24U
#define SOLAR_OS_JOB_RESOURCE_MAX 4U
#define SOLAR_OS_JOB_RESOURCE_NAME_MAX 64U
#define SOLAR_OS_JOB_RESOURCE_DETAIL_MAX 32U
#define SOLAR_OS_JOB_ERROR_DETAIL_MAX 128U
#define SOLAR_OS_NATIVE_JOB_BACKGROUND 0U
#define SOLAR_OS_NATIVE_JOB_INTERACTIVE 1U

typedef enum {
    SOLAR_OS_JOB_STOPPED,
    SOLAR_OS_JOB_WAITING,
    SOLAR_OS_JOB_RUNNING,
    SOLAR_OS_JOB_FAILED,
} solar_os_job_state_t;

typedef enum {
    SOLAR_OS_JOB_RESOURCE_NONE,
    SOLAR_OS_JOB_RESOURCE_PORT,
    SOLAR_OS_JOB_RESOURCE_FILE,
    SOLAR_OS_JOB_RESOURCE_NET,
    SOLAR_OS_JOB_RESOURCE_STREAM,
    SOLAR_OS_JOB_RESOURCE_CUSTOM,
} solar_os_job_resource_type_t;

typedef struct {
    solar_os_job_resource_type_t type;
    char name[SOLAR_OS_JOB_RESOURCE_NAME_MAX];
    char detail[SOLAR_OS_JOB_RESOURCE_DETAIL_MAX];
} solar_os_job_resource_t;

typedef struct {
    uint32_t interval_ms, deadline_ms, dispatch_count, deadline_miss_count;
    uint32_t last_dispatch_ms, last_duration_us, max_duration_us;
} solar_os_job_tick_stats_v1_t;

typedef struct {
    char name[SOLAR_OS_JOB_NAME_MAX], summary[SOLAR_OS_JOB_SUMMARY_MAX];
    uint32_t kind;
    solar_os_job_state_t state;
    int32_t last_error;
    uint32_t tick_count, last_tick_ms, generation, worker_stack_bytes;
    bool has_event, has_detail, worker_stack_external;
    solar_os_job_tick_stats_v1_t tick_stats;
    char owner[SOLAR_OS_JOB_OWNER_MAX];
    uint32_t resource_count;
    solar_os_job_resource_t resources[SOLAR_OS_JOB_RESOURCE_MAX];
} solar_os_job_status_v1_t;

typedef enum {
    SOLAR_OS_JOB_START_READY,
    SOLAR_OS_JOB_START_RUNNING,
    SOLAR_OS_JOB_START_WAITING_MEMORY,
    SOLAR_OS_JOB_START_BLOCKED,
} solar_os_job_start_disposition_t;

typedef enum {
    SOLAR_OS_JOB_START_REASON_NONE,
    SOLAR_OS_JOB_START_REASON_ALREADY_RUNNING,
    SOLAR_OS_JOB_START_REASON_WORKER_STACK_MEMORY,
    SOLAR_OS_JOB_START_REASON_LIFECYCLE_BUSY,
    SOLAR_OS_JOB_START_REASON_NOT_STARTABLE,
} solar_os_job_start_reason_t;

typedef struct {
    solar_os_job_status_v1_t status;
    solar_os_job_start_disposition_t disposition;
    solar_os_job_start_reason_t reason;
} solar_os_job_inspection_v1_t;

/* Job records contain copied values. Admission is advisory and can change
 * immediately. A successful start can enter WAITING until memory is
 * available. The caller supplies writable argv strings, with the job name as
 * argv[0] when argc is nonzero. Queued starts retain a copy. Detail output
 * uses the calling application's shell I/O and requires its active context. */
typedef struct {
    uint32_t abi_version, struct_size;
    size_t (*count)(void);
    int (*info)(size_t index, solar_os_job_inspection_v1_t *entry);
    int (*find)(const char *name, solar_os_job_inspection_v1_t *entry);
    int (*find_owner)(const char *owner, solar_os_job_inspection_v1_t *entry);
    int (*start)(const char *name, int argc, char **argv);
    int (*stop)(const char *name);
    bool (*error_detail)(const char *name, char *buffer, size_t capacity);
    int (*print_detail)(const char *name);
    const char *(*state_name)(solar_os_job_state_t state);
    const char *(*kind_name)(uint32_t kind);
    const char *(*resource_type_name)(solar_os_job_resource_type_t type);
    const char *(*start_disposition_name)(solar_os_job_start_disposition_t disposition);
    const char *(*start_reason_name)(solar_os_job_start_reason_t reason);
} solar_os_native_jobs_api_v1_t;
