#pragma once
#include "solar_os_native_service_abi.h"
#define SOLAR_OS_NATIVE_FS_SERVICE "fs"
#define SOLAR_OS_NATIVE_FS_ABI 1U
#define SOLAR_OS_NATIVE_FS_FILE 1U
#define SOLAR_OS_NATIVE_FS_DIRECTORY 2U
#define SOLAR_OS_NATIVE_FS_OTHER 3U
#define SOLAR_OS_NATIVE_FS_PATH_MAX 160U
#define SOLAR_OS_NATIVE_FS_IO_MAX 65536U
#define SOLAR_OS_NATIVE_FS_SCAN_MAX 128U
#define SOLAR_OS_NATIVE_FS_REPLACE_MAX 32U
typedef struct {
    uint64_t bytes;
    int64_t modified_seconds;
    uint32_t mode, type;
} solar_os_native_fs_metadata_v1_t;
typedef struct {
    char name[160];
    solar_os_native_fs_metadata_v1_t metadata;
} solar_os_native_fs_entry_v1_t;
typedef struct {
    const char *staged, *active, *backup;
} solar_os_native_fs_replacement_v1_t;
typedef void (*solar_os_native_fs_progress_fn)(uint64_t done, uint64_t total, void *user);

/* Paths are resolved absolute paths except in explicit path helpers. Results
 * belong to the caller. No argument is retained. Copy callbacks run only on
 * the calling task during copy(), never after return. Copy callbacks must not
 * change filesystem topology or mutate the source/destination. Operations use
 * the OS's existing image-ownership and recoverable-replacement policies.
 * read_at/write_at open, perform one bounded operation, and close before
 * returning; they do not keep a stream or pin a mount between calls. Writes
 * sync before success; failures may leave partial output. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*resolve)(const char *cwd, const char *argument, char *path, size_t capacity);
    int (*normalize)(const char *path, char *out, size_t capacity);
    int (*join)(const char *base, const char *relative, char *out, size_t capacity);
    int (*default_path)(const char *relative, char *out, size_t capacity);
    int (*sibling)(const char *path, const char *suffix, char *out, size_t capacity);
    int (*mount_point)(const char *path, char *out, size_t capacity);
    bool (*has_mount_prefix)(const char *path);
    int (*stat)(const char *path, solar_os_native_fs_metadata_v1_t *metadata);
    int (*exists)(const char *path, bool *exists);
    int (*scandir)(const char *path, uint64_t cursor, uint32_t limit,
        solar_os_native_fs_entry_v1_t *entries, uint32_t *count, uint64_t *next, bool *more);
    int (*mkdir)(const char *path, bool parents, bool exist_ok);
    int (*remove)(const char *path, bool directory);
    int (*rename)(const char *from, const char *to);
    int (*read)(const char *path, void *bytes, size_t capacity, size_t *read_bytes);
    int (*write)(const char *path, const void *bytes, size_t size, bool append);
    int (*read_at)(const char *path, uint64_t offset, void *bytes, size_t capacity, size_t *read_bytes);
    int (*write_at)(const char *path, uint64_t offset, const void *bytes, size_t size, bool create, size_t *written);
    int (*truncate)(const char *path, uint64_t size);
    int (*sync)(const char *path);
    int (*replace)(const solar_os_native_fs_replacement_v1_t *items, uint32_t count);
    int (*copy)(const char *from, const char *to, solar_os_native_fs_progress_fn progress,
        solar_os_native_cancel_fn cancel, void *user);
    uint32_t (*revision)(void);
    void (*notify_changed)(void);
} solar_os_native_fs_api_v1_t;
