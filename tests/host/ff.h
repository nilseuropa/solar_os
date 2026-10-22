#pragma once

#include <stdint.h>

#define FF_MAX_SS 512
#define FF_MIN_SS 512
#define FR_OK 0
#define FR_DISK_ERR 1
#define FR_INT_ERR 2
#define FR_NOT_READY 3
#define FR_NO_FILE 4
#define FR_NO_PATH 5
#define FR_INVALID_NAME 6
#define FR_DENIED 7
#define FR_EXIST 8
#define FR_INVALID_OBJECT 9
#define FR_WRITE_PROTECTED 10
#define FR_INVALID_DRIVE 11
#define FR_NOT_ENABLED 12
#define FR_NO_FILESYSTEM 13
#define FR_MKFS_ABORTED 14
#define FR_TIMEOUT 15
#define FR_LOCKED 16
#define FR_NOT_ENOUGH_CORE 17
#define FR_TOO_MANY_OPEN_FILES 18
#define FR_INVALID_PARAMETER 19

typedef uint32_t DWORD;
typedef int FRESULT;

typedef struct {
    uint32_t csize;
    uint32_t n_fatent;
} FATFS;

FRESULT f_getfree(const char *path, DWORD *free_clusters, FATFS **fs);
