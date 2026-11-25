#pragma once
/* Firmware service substrate. This header has no ELF loader dependency. */
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "solar_os_native_abi.h"
#include "solar_os_native_app_abi.h"
#include "solar_os_native_library_abi.h"
#include "solar_os_native_job_abi.h"
#include "solar_os_native_driver_abi.h"
#include "solar_os_components.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef esp_err_t (*solar_os_native_write_fn)(const char *text,
                                              size_t text_len,
                                              void *user);

typedef struct {
    solar_os_native_write_fn write;
    void *write_user;
    const void *(*get_service)(const char *name, uint32_t abi,
                               uint32_t minimum_size, void *user);
    void *service_user;
    /* Runs on the module's task before removing its scope or unloading ELF. */
    void (*cleanup)(void *user);
    const solar_os_native_app_manifest_v2_t *expected_manifest;
} solar_os_native_run_options_t;

/* Only the calling execution task can obtain its host-owned service state. */
void *solar_os_native_run_service_user(void);

/* Firmware capabilities only; does not create or expose a caller's scope. */
bool solar_os_native_service_available(const char *name, uint32_t abi, uint32_t minimum_size);

typedef struct native_run_scope {
    struct native_run_scope *next;
    solar_os_native_run_options_t options;
} native_run_scope_t;


void solar_os_system_scope_enter(native_run_scope_t *scope);
void solar_os_system_scope_leave(native_run_scope_t *scope);
/* Core lifecycle registration endpoints, shared by compiled and loaded components. */
int solar_os_native_register_job(const solar_os_native_job_descriptor_v1_t *descriptor);
int solar_os_native_register_driver(const solar_os_native_driver_descriptor_v1_t *descriptor);
int solar_os_native_register_library(const solar_os_native_library_descriptor_v1_t *descriptor);
int solar_os_native_library_acquire(const char *id, uint32_t abi, uint32_t size, uint32_t *lease, const void **api);
int solar_os_native_library_retain(uint32_t lease, uint32_t *retained);
int solar_os_native_library_release(uint32_t lease);
int solar_os_native_library_unload(const char *id);
esp_err_t solar_os_native_run_builtin(int (*entry)(int, char **), int argc,
    char **argv, const solar_os_native_run_options_t *options, int *exit_status);
/* Only the core foreground host calls this after admitting the component's
 * immutable manifest and creating its session resources. */
esp_err_t solar_os_native_run_compiled_app(const solar_os_component_t *component,
    int argc, char **argv, const solar_os_native_run_options_t *options, int *exit_status);
#ifdef __cplusplus
}
#endif
