#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os.h"
#include "solar_os_config.h"
#include "solar_os_port.h"
#include "solar_os_scheduler.h"

#define SOLAR_OS_JOB_OWNER_MAX SOLAR_OS_PORT_OWNER_MAX
#define SOLAR_OS_JOB_RESOURCE_MAX 4
#define SOLAR_OS_JOB_RESOURCE_NAME_MAX 64
#define SOLAR_OS_JOB_RESOURCE_DETAIL_MAX 32
#define SOLAR_OS_JOB_ERROR_DETAIL_MAX 128

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
    const char *name;
    const char *summary;
    solar_os_job_kind_t kind;
    solar_os_job_state_t state;
    esp_err_t last_error;
    uint32_t tick_count;
    uint32_t last_tick_ms;
    uint32_t generation;
    bool has_event;
    void (*detail)(solar_os_context_t *ctx);
    uint32_t worker_stack_bytes;
    bool worker_stack_external;
    solar_os_tick_stats_t tick_stats;
    char owner[SOLAR_OS_JOB_OWNER_MAX];
    size_t resource_count;
    solar_os_job_resource_t resources[SOLAR_OS_JOB_RESOURCE_MAX];
} solar_os_job_status_t;

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
    solar_os_job_status_t status;
    solar_os_job_start_disposition_t disposition;
    solar_os_job_start_reason_t reason;
} solar_os_job_inspection_t;

esp_err_t solar_os_jobs_init(void);
size_t solar_os_jobs_count(void);
bool solar_os_jobs_get(size_t index, solar_os_job_status_t *status);
bool solar_os_jobs_get_by_name(const char *name, solar_os_job_status_t *status);
bool solar_os_jobs_get_by_owner(const char *owner, solar_os_job_status_t *status);
bool solar_os_jobs_inspect(size_t index,
                           solar_os_job_inspection_t *inspection);
bool solar_os_jobs_inspect_by_name(
    const char *name,
    solar_os_job_inspection_t *inspection);
esp_err_t solar_os_jobs_start(solar_os_context_t *ctx, const char *name, int argc, char **argv);
esp_err_t solar_os_jobs_stop(solar_os_context_t *ctx, const char *name);
/* Capture during start; asynchronous completion must present the same generation. */
esp_err_t solar_os_jobs_get_generation(const char *name, uint32_t *generation);
esp_err_t solar_os_jobs_mark_stopped(const char *name,
                                     uint32_t generation,
                                     esp_err_t last_error);
uint32_t solar_os_jobs_requested_tick_interval_ms(void);
void solar_os_jobs_tick(solar_os_context_t *ctx, uint32_t now_ms);
esp_err_t solar_os_jobs_owner_name(const char *name, char *owner, size_t owner_len);
esp_err_t solar_os_jobs_note_resource(const char *name,
                                      solar_os_job_resource_type_t type,
                                      const char *resource,
                                      const char *detail);
bool solar_os_jobs_get_error_detail(const char *name,
                                    char *buffer,
                                    size_t buffer_len);
void solar_os_jobs_clear_resources(const char *name);
esp_err_t solar_os_jobs_claim_port(const char *name,
                                   const char *port_name,
                                   solar_os_port_handle_t *handle);
const char *solar_os_job_state_name(solar_os_job_state_t state);
const char *solar_os_job_kind_name(solar_os_job_kind_t kind);
const char *solar_os_job_resource_type_name(solar_os_job_resource_type_t type);
const char *solar_os_job_start_disposition_name(
    solar_os_job_start_disposition_t disposition);
const char *solar_os_job_start_reason_name(
    solar_os_job_start_reason_t reason);
