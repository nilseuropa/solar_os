#pragma once
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "esp_err.h"
struct __dirstream { int vfs_index; };
#define ESP_VFS_FLAG_CONTEXT_PTR 1
typedef struct {
    int flags;
    int (*open_p)(void *, const char *, int, int);
    int (*close_p)(void *, int);
    ssize_t (*read_p)(void *, int, void *, size_t);
    ssize_t (*write_p)(void *, int, const void *, size_t);
    off_t (*lseek_p)(void *, int, off_t, int);
    int (*fsync_p)(void *, int);
    int (*fstat_p)(void *, int, struct stat *);
    int (*stat_p)(void *, const char *, struct stat *);
    int (*unlink_p)(void *, const char *);
    int (*rmdir_p)(void *, const char *);
    int (*mkdir_p)(void *, const char *, mode_t);
    int (*rename_p)(void *, const char *, const char *);
    DIR *(*opendir_p)(void *, const char *);
    struct dirent *(*readdir_p)(void *, DIR *);
    int (*readdir_r_p)(void *, DIR *, struct dirent *, struct dirent **);
    int (*closedir_p)(void *, DIR *);
} esp_vfs_t;
esp_err_t esp_vfs_register(const char *path, const esp_vfs_t *vfs, void *ctx);
