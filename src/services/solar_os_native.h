#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_module_packages.h"
#include "solar_os_native_abi.h"
#include "solar_os_native_elf.h"
#include "solar_os_native_library_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_NATIVE_ELF_MAX_BYTES (2U * 1024U * 1024U)

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

typedef struct {
    solar_os_native_elf_info_t elf;
} solar_os_native_run_result_t;

esp_err_t solar_os_native_run(const char *path,
                              int argc,
                              char **argv,
                              const solar_os_native_run_options_t *options,
                              solar_os_native_run_result_t *result,
                              char *detail,
                              size_t detail_len);
esp_err_t solar_os_native_module_activate(solar_os_module_type_t type,
                                          const char *id,
                                          const char *path,
                                          char *detail,
                                          size_t detail_len);
esp_err_t solar_os_native_module_deactivate(solar_os_module_type_t type,
                                            const char *id,
                                            char *detail,
                                            size_t detail_len);
bool solar_os_native_module_active(solar_os_module_type_t type,
                                   const char *id);

/* Stable native service results, usable by firmware consumers as well. */
int solar_os_native_library_acquire(const char *id, uint32_t abi_version,
                                    uint32_t minimum_struct_size,
                                    uint32_t *lease, const void **api);
int solar_os_native_library_retain(uint32_t lease, uint32_t *retained_lease);
int solar_os_native_library_release(uint32_t lease);
int solar_os_native_library_unload(const char *id);

#ifdef __cplusplus
}
#endif
