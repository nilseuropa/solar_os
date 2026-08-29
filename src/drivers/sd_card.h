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
#define SD_CARD_DIAGNOSTIC_OPERATION_MAX 12

typedef enum {
    SD_CARD_KIND_SDSC,
    SD_CARD_KIND_SDHC,
    SD_CARD_KIND_SDHC_UHS1,
    SD_CARD_KIND_SDIO,
    SD_CARD_KIND_MMC,
} sd_card_kind_t;

typedef struct {
    bool attempted;
    bool card_initialized;
    sd_card_kind_t kind;
    bool ddr;
    char name[8];
    uint32_t real_freq_khz;
    uint32_t max_freq_khz;
    uint64_t size_bytes;
    int csd_version;
    int sector_size;
    int capacity_sectors;
    int read_block_len;
    uint32_t bus_width;
    esp_err_t init_error;
    esp_err_t mount_error;
    esp_err_t diskio_error;
    char diskio_operation[SD_CARD_DIAGNOSTIC_OPERATION_MAX];
    int fatfs_result;
} sd_card_diagnostics_t;

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
esp_err_t sd_card_configure_sdmmc(int clk_pin,
                                  int cmd_pin,
                                  int d0_pin,
                                  int d1_pin,
                                  int d2_pin,
                                  int d3_pin);
esp_err_t sd_card_clear_sdmmc_config(void);
bool sd_card_configured(void);
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
bool sd_card_get_last_diagnostics(sd_card_diagnostics_t *diagnostics);
