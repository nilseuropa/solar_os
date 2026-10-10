#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct solar_os_context solar_os_context_t;
typedef struct solar_os_event solar_os_event_t;
typedef enum { SOLAR_OS_JOB_KIND_BACKGROUND } solar_os_job_kind_t;
typedef enum { SOLAR_OS_JOB_STOPPED, SOLAR_OS_JOB_WAITING, SOLAR_OS_JOB_RUNNING, SOLAR_OS_JOB_FAILED } solar_os_job_state_t;
typedef enum { SOLAR_OS_JOB_RESOURCE_CUSTOM, SOLAR_OS_JOB_RESOURCE_FILE, SOLAR_OS_JOB_RESOURCE_NET, SOLAR_OS_JOB_RESOURCE_STREAM } solar_os_job_resource_type_t;
typedef struct {
    const char *name, *summary;
    solar_os_job_kind_t kind;
    void *callback_user;
    esp_err_t (*start_with_user)(void *, solar_os_context_t *, int, char **);
    void (*stop_with_user)(void *, solar_os_context_t *);
    bool (*event_with_user)(void *, solar_os_context_t *, const solar_os_event_t *);
    uint32_t worker_stack_bytes;
} solar_os_job_t;
typedef struct { solar_os_job_state_t state; esp_err_t last_error; } solar_os_job_status_t;
esp_err_t solar_os_jobs_register_dynamic(const char *, const char *, const solar_os_job_t *);
esp_err_t solar_os_jobs_unregister_dynamic(const char *, const solar_os_job_t *);
esp_err_t solar_os_jobs_start(solar_os_context_t *, const char *, int, char **);
esp_err_t solar_os_jobs_stop(solar_os_context_t *, const char *);
esp_err_t solar_os_jobs_get_generation(const char *, uint32_t *);
esp_err_t solar_os_jobs_mark_stopped(const char *, uint32_t, esp_err_t);
esp_err_t solar_os_jobs_note_resource(const char *, solar_os_job_resource_type_t, const char *, const char *);
bool solar_os_jobs_get_by_name(const char *, solar_os_job_status_t *);
const char *solar_os_job_state_name(solar_os_job_state_t);
