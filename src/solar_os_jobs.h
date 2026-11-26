#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os.h"
#include "solar_os_config.h"
#include "solar_os_native_jobs_abi.h"
#include "solar_os_port.h"
#include "solar_os_scheduler.h"

typedef solar_os_job_status_v1_t solar_os_job_status_t;
typedef solar_os_job_inspection_v1_t solar_os_job_inspection_t;

esp_err_t solar_os_jobs_init(void);
esp_err_t solar_os_jobs_register_dynamic(const char *name,
                                         const char *summary,
                                         const solar_os_job_t *job);
esp_err_t solar_os_jobs_unregister_dynamic(const char *name,
                                           const solar_os_job_t *job);
size_t solar_os_jobs_count(void);
bool solar_os_jobs_get(size_t index, solar_os_job_status_t *status);
bool solar_os_jobs_get_by_name(const char *name, solar_os_job_status_t *status);
bool solar_os_jobs_get_by_owner(const char *owner, solar_os_job_status_t *status);
bool solar_os_jobs_inspect(size_t index,
                           solar_os_job_inspection_t *inspection);
bool solar_os_jobs_inspect_by_name(
    const char *name,
    solar_os_job_inspection_t *inspection);
bool solar_os_jobs_inspect_by_owner(const char *owner,
    solar_os_job_inspection_t *inspection);
esp_err_t solar_os_jobs_start(solar_os_context_t *ctx, const char *name, int argc, char **argv);
esp_err_t solar_os_jobs_stop(solar_os_context_t *ctx, const char *name);
esp_err_t solar_os_jobs_prepare_shutdown(solar_os_context_t *ctx);
/* Capture during start; asynchronous completion must present the same generation. */
esp_err_t solar_os_jobs_get_generation(const char *name, uint32_t *generation);
esp_err_t solar_os_jobs_mark_stopped(const char *name,
                                     uint32_t generation,
                                     esp_err_t last_error);
uint32_t solar_os_jobs_next_tick_wait_ms(uint32_t now_ms);
void solar_os_jobs_tick(solar_os_context_t *ctx, uint32_t now_ms);
esp_err_t solar_os_jobs_owner_name(const char *name, char *owner, size_t owner_len);
esp_err_t solar_os_jobs_note_resource(const char *name,
                                      solar_os_job_resource_type_t type,
                                      const char *resource,
                                      const char *detail);
bool solar_os_jobs_get_error_detail(const char *name,
                                    char *buffer,
                                    size_t buffer_len);
esp_err_t solar_os_jobs_print_detail(solar_os_context_t *ctx, const char *name);
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
