#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SD_CARD_MOUNT_POINT "/sdcard"
#define SD_CARD_MAX_BLOCKS 9
#define SD_CARD_BLOCK_NAME_MAX 12
#define SD_CARD_FS_NAME_MAX 8
#define SD_CARD_TYPE_NAME_MAX 12
#define SD_CARD_MOUNT_POINT_MAX 32
#define SD_CARD_LOGICAL_VOLUME_INVALID UINT8_MAX

typedef enum {
    SD_CARD_BLOCK_DISK,
    SD_CARD_BLOCK_PARTITION,
} sd_card_block_type_t;

typedef struct {
    char name[SD_CARD_BLOCK_NAME_MAX];
    sd_card_block_type_t type;
    uint8_t partition_number;
    uint8_t mbr_type;
    bool bootable;
    bool mountable;
    bool mounted;
    bool whole_disk_filesystem;
    uint8_t logical_volume;
    uint64_t start_sector;
    uint64_t sector_count;
    uint32_t sector_size;
    uint64_t size_bytes;
    char fs[SD_CARD_FS_NAME_MAX];
    char type_name[SD_CARD_TYPE_NAME_MAX];
    char mount_point[SD_CARD_MOUNT_POINT_MAX];
} sd_card_block_t;

esp_err_t sd_card_init(void);
esp_err_t sd_card_configure_sdspi(int host, int cs_pin);
esp_err_t sd_card_clear_sdspi_config(void);
bool sd_card_sdspi_configured(void);
esp_err_t sd_card_unmount(void);
esp_err_t sd_card_mount_volume(const char *name, const char *mount_point);
esp_err_t sd_card_unmount_volume(const char *target);
esp_err_t sd_card_format(const char *name);
bool sd_card_is_mounted(void);
bool sd_card_has_mounts(void);
void sd_card_get_status(char *buffer, size_t len);
const char *sd_card_mount_point(void);
esp_err_t sd_card_rescan(void);
size_t sd_card_block_count(void);
bool sd_card_get_block(size_t index, sd_card_block_t *block);
