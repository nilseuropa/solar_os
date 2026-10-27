#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_native_app_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_NATIVE_ELF_MACHINE_XTENSA 94U
#define SOLAR_OS_NATIVE_ELF_MACHINE_RISCV 243U

typedef struct {
    size_t file_size;
    uint32_t entry;
    uint16_t machine;
    uint16_t program_count;
    uint16_t section_count;
    bool foreground;
    solar_os_native_app_manifest_v2_t app;
} solar_os_native_elf_info_t;

/* Read exactly length bytes at offset, or return an error. Discovery reads
 * only ELF metadata and the app manifest; execution still requires validate. */
typedef esp_err_t (*solar_os_native_elf_read_fn)(void *context, size_t offset,
                                                void *buffer, size_t length);
esp_err_t solar_os_native_elf_discover(solar_os_native_elf_read_fn read,
                                       void *context, size_t file_size,
                                       uint16_t expected_machine,
                                       solar_os_native_elf_info_t *info,
                                       char *detail, size_t detail_len);

esp_err_t solar_os_native_elf_validate(const uint8_t *data,
                                       size_t data_len,
                                       uint16_t expected_machine,
                                       solar_os_native_elf_info_t *info,
                                       char *detail,
                                       size_t detail_len);

const char *solar_os_native_elf_machine_name(uint16_t machine);

#ifdef __cplusplus
}
#endif
