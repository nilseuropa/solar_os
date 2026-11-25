#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_native_runtime.h"
#include "solar_os_native_abi.h"
#include "solar_os_native_elf.h"
#include "solar_os_native_library_abi.h"
#include "solar_os_system_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_NATIVE_ELF_MAX_BYTES (2U * 1024U * 1024U)

typedef struct {
    solar_os_native_elf_info_t elf;
} solar_os_native_run_result_t;

/* Run trusted firmware code with the same task-local services and cleanup as
 * an ELF command. Its entry status is returned separately from setup errors. */
esp_err_t solar_os_native_run_builtin(int (*entry)(int, char **), int argc,
                                      char **argv,
                                      const solar_os_native_run_options_t *options,
                                      int *exit_status);

esp_err_t solar_os_native_run(const char *path,
                              int argc,
                              char **argv,
                              const solar_os_native_run_options_t *options,
                              solar_os_native_run_result_t *result,
                              char *detail,
                              size_t detail_len);
#ifdef __cplusplus
}
#endif
