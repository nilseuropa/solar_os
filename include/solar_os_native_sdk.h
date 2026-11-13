#pragma once
#include <stdint.h>

/* Non-allocated ELF section, covered by the installed artifact's signature/hash.
 * All integers are little-endian. Text and paths exclude terminating NULs in
 * their sizes; fixed-width names are NUL terminated and padded with zeroes. */
#define SOLAR_OS_NATIVE_SDK_SECTION ".solaros.sdk"
#define SOLAR_OS_NATIVE_SDK_MAGIC "SOLSDK1"
#define SOLAR_OS_NATIVE_SDK_HEADER_BYTES 96U
#define SOLAR_OS_NATIVE_SDK_RECORD_BYTES 104U
#define SOLAR_OS_NATIVE_SDK_FILES_MAX 32U
#define SOLAR_OS_NATIVE_SDK_FILE_MAX 32768U
#define SOLAR_OS_NATIVE_SDK_TEXT_MAX 262144U
/* Header: magic[8], total_bytes, file_count, module_id[48], version[32].
 * Record: namespaced_path[96], data_offset, data_bytes. Offsets are relative
 * to the section, and the data follows the complete record table. */
