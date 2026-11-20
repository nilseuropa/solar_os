#pragma once
#include "solar_os_cc_abi.h"
#include "solar_os_native_storage_abi.h"
#include "solar_os_native_time_abi.h"

#define SOLAR_OS_CC_PROJECT_BYTES_MAX 8192U
#define SOLAR_OS_CC_PROJECT_DEPENDENCIES_MAX 8U

typedef struct {
    char id[48], version[32];
} solar_os_cc_project_dependency_t;
typedef struct {
    char sources[SOLAR_OS_CC_SOURCE_FILES_MAX][SOLAR_OS_NATIVE_STORAGE_PATH_MAX];
    char include_dirs[8][SOLAR_OS_NATIVE_STORAGE_PATH_MAX];
    char output[SOLAR_OS_NATIVE_STORAGE_PATH_MAX];
    solar_os_cc_project_dependency_t dependencies[SOLAR_OS_CC_PROJECT_DEPENDENCIES_MAX];
    unsigned source_count, include_count, dependency_count;
} solar_os_cc_project_t;

/* Bounded TOML subset: root string arrays, output string, inline dependency
 * tables. No allocation, recursion, command execution or filesystem changes. */
int solar_os_cc_project_parse(const char *text, size_t bytes, solar_os_cc_project_t *project,
    char *detail, size_t capacity);
/* Resolve paths relative to the manifest and check installed SDK metadata.
 * Providers are not executed. No output files/directories are changed here. */
int solar_os_cc_project_load(const solar_os_native_storage_api_v1_t *storage,
    const solar_os_native_memory_api_v1_t *memory, const solar_os_native_time_api_v1_t *clock,
    const char *manifest, solar_os_cc_project_t *project, char *detail, size_t capacity);
bool solar_os_cc_project_same_path(const char *a, const char *b);
