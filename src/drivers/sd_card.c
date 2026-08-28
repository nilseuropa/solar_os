#include "sd_card.h"
#include "solar_os_config.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#if SOLAR_OS_BOARD_STORAGE_SDSPI || \
    (SOLAR_OS_PACKAGE_EXPANSION_SDSPI && !SOLAR_OS_BOARD_HAS_SD)
#include "driver/sdspi_host.h"
#endif
#if SOLAR_OS_BOARD_STORAGE_SDSPI
#include "spi_bus.h"
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_SDMMC
#include "driver/sdmmc_host.h"
#endif
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "flash_storage.h"
#include "sdmmc_cmd.h"
#include "sdkconfig.h"
#include "solar_os_board.h"

#ifndef SOLAR_OS_PACKAGE_EXPANSION_SDSPI
#error "solar_os_config.h must define SOLAR_OS_PACKAGE_EXPANSION_SDSPI"
#endif
#ifndef SOLAR_OS_PACKAGE_EXPANSION_SDMMC
#error "solar_os_config.h must define SOLAR_OS_PACKAGE_EXPANSION_SDMMC"
#endif

#define SD_CARD_MAX_MOUNTS FF_VOLUMES
#define SD_CARD_SECTOR_BYTES 512U
#define SD_CARD_DEFAULT_MAX_FILES 5
#define SD_CARD_EXTRA_MAX_FILES 3
#define SD_CARD_ALLOC_UNIT_SIZE (16 * 1024)

#define SD_CARD_HAS_SDSPI (SOLAR_OS_BOARD_STORAGE_SDSPI || \
                           (SOLAR_OS_PACKAGE_EXPANSION_SDSPI && !SOLAR_OS_BOARD_HAS_SD))

typedef enum {
    SD_CARD_TRANSPORT_NONE,
    SD_CARD_TRANSPORT_SDSPI_BOARD,
    SD_CARD_TRANSPORT_SDSPI_RUNTIME,
    SD_CARD_TRANSPORT_SDMMC,
} sd_card_transport_t;

typedef struct {
    bool active;
    uint8_t logical_volume;
    uint8_t partition_number;
    FATFS *fs;
    char block_name[SD_CARD_BLOCK_NAME_MAX];
    char mount_point[SD_CARD_MOUNT_POINT_MAX];
} sd_card_mount_t;

static const char *TAG = "sd_card";

static sdmmc_card_t card_storage;
static sdmmc_card_t *card;
static sdmmc_host_t host;
#if SD_CARD_HAS_SDSPI
static bool sdspi_device_ready;
#endif
#if SOLAR_OS_BOARD_STORAGE_SDSPI
static bool sdspi_bus_acquired;
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_SDSPI && !SOLAR_OS_BOARD_HAS_SD
static bool sdspi_runtime_configured;
static int sdspi_runtime_host = -1;
static int sdspi_runtime_cs = -1;
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_SDMMC
static int sdmmc_pins[6] = {-1, -1, -1, -1, -1, -1};
#endif
#if SOLAR_OS_BOARD_STORAGE_SDSPI
static sd_card_transport_t transport = SD_CARD_TRANSPORT_SDSPI_BOARD;
#else
static sd_card_transport_t transport = SD_CARD_TRANSPORT_NONE;
#endif
static BYTE physical_pdrv = FF_DRV_NOT_USED;
static bool card_ready;
static bool diskio_registered;
static char status_text[64] = "not mounted";
static sd_card_block_t blocks[SD_CARD_MAX_BLOCKS];
static size_t block_count;
static sd_card_mount_t mounts[SD_CARD_MAX_MOUNTS];

static bool diagnostics_attempted;
static bool diagnostics_card_initialized;
static esp_err_t diagnostics_init_error = ESP_OK;
static esp_err_t diagnostics_mount_error = ESP_OK;
static esp_err_t diagnostics_diskio_error = ESP_OK;
static char diagnostics_diskio_operation[12];
static FRESULT diagnostics_fresult = FR_OK;

static bool sd_card_has_active_mounts(void);

#if SOLAR_OS_PACKAGE_EXPANSION_SDSPI && !SOLAR_OS_BOARD_HAS_SD
static void diagnostics_note_diskio_error(const char *operation, esp_err_t err)
{
    if (err == ESP_OK) {
        return;
    }
    diagnostics_diskio_error = err;
    strlcpy(diagnostics_diskio_operation,
            operation != NULL ? operation : "I/O",
            sizeof(diagnostics_diskio_operation));
    ESP_LOGE(TAG,
             "SD %s failed (0x%x)",
             diagnostics_diskio_operation,
             (unsigned)err);
}

static DSTATUS sd_card_disk_initialize(BYTE pdrv)
{
    (void)pdrv;
    const esp_err_t err = sdmmc_get_status(&card_storage);
    diagnostics_note_diskio_error("status", err);
    return err == ESP_OK ? 0 : STA_NOINIT;
}

static DSTATUS sd_card_disk_status(BYTE pdrv)
{
    (void)pdrv;
    return 0;
}

static DRESULT sd_card_disk_read(BYTE pdrv, BYTE *buffer, DWORD sector, UINT count)
{
    (void)pdrv;
    const esp_err_t err = sdmmc_read_sectors(&card_storage, buffer, sector, count);
    diagnostics_note_diskio_error("read", err);
    return err == ESP_OK ? RES_OK : RES_ERROR;
}

static DRESULT sd_card_disk_write(BYTE pdrv, const BYTE *buffer, DWORD sector, UINT count)
{
    (void)pdrv;
    const esp_err_t err = sdmmc_write_sectors(&card_storage, buffer, sector, count);
    diagnostics_note_diskio_error("write", err);
    return err == ESP_OK ? RES_OK : RES_ERROR;
}

#if FF_USE_TRIM
static DRESULT sd_card_disk_trim(DWORD start_sector, DWORD sector_count)
{
    const sdmmc_erase_arg_t argument =
        sdmmc_can_discard(&card_storage) == ESP_OK ? SDMMC_DISCARD_ARG : SDMMC_ERASE_ARG;
    const esp_err_t err = sdmmc_erase_sectors(&card_storage,
                                               start_sector,
                                               sector_count,
                                               argument);
    diagnostics_note_diskio_error("erase", err);
    return err == ESP_OK ? RES_OK : RES_ERROR;
}
#endif

static DRESULT sd_card_disk_ioctl(BYTE pdrv, BYTE command, void *buffer)
{
    (void)pdrv;
    switch (command) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(DWORD *)buffer = card_storage.csd.capacity;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buffer = card_storage.csd.sector_size;
        return RES_OK;
#if FF_USE_TRIM
    case CTRL_TRIM:
        if (sdmmc_can_trim(&card_storage) != ESP_OK) {
            return RES_PARERR;
        }
        return sd_card_disk_trim(*(DWORD *)buffer,
                                 *((DWORD *)buffer + 1) - *(DWORD *)buffer + 1U);
#endif
    default:
        return RES_ERROR;
    }
}

static const ff_diskio_impl_t sd_card_diskio = {
    .init = sd_card_disk_initialize,
    .status = sd_card_disk_status,
    .read = sd_card_disk_read,
    .write = sd_card_disk_write,
    .ioctl = sd_card_disk_ioctl,
};
#endif

static uint16_t get_u16le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t get_u32le(const uint8_t *data)
{
    return (uint32_t)data[0] |
        ((uint32_t)data[1] << 8) |
        ((uint32_t)data[2] << 16) |
        ((uint32_t)data[3] << 24);
}

static uint64_t get_u64le(const uint8_t *data)
{
    return (uint64_t)get_u32le(data) | ((uint64_t)get_u32le(data + 4) << 32);
}

static bool guid_is_zero(const uint8_t *guid)
{
    for (size_t i = 0; i < 16; i++) {
        if (guid[i] != 0) {
            return false;
        }
    }
    return true;
}

static void set_mount_error_status(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_TIMEOUT:
    case ESP_ERR_NOT_FOUND:
        snprintf(status_text, sizeof(status_text), "no card");
        break;
    case ESP_FAIL:
        snprintf(status_text, sizeof(status_text), "mount failed");
        break;
    default:
        snprintf(status_text, sizeof(status_text), "error %s", esp_err_to_name(err));
        break;
    }
}

static void sd_card_deinit_host(void)
{
#if SD_CARD_HAS_SDSPI
    if (transport == SD_CARD_TRANSPORT_SDSPI_BOARD ||
        transport == SD_CARD_TRANSPORT_SDSPI_RUNTIME) {
        if (sdspi_device_ready) {
            host.deinit_p(host.slot);
            sdspi_device_ready = false;
        }
#if SOLAR_OS_BOARD_STORAGE_SDSPI
        if (sdspi_bus_acquired) {
            solar_os_spi_bus_release();
            sdspi_bus_acquired = false;
        }
#endif
        return;
    }
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_SDMMC
    if (transport == SD_CARD_TRANSPORT_SDMMC) {
        if (host.flags & SDMMC_HOST_FLAG_DEINIT_ARG) {
            host.deinit_p(host.slot);
        } else {
            host.deinit();
        }
    }
#endif
}

#if SOLAR_OS_PACKAGE_EXPANSION_SDMMC
static void sd_card_make_slot_config(sdmmc_slot_config_t *slot_config)
{
    *slot_config = (sdmmc_slot_config_t)SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config->width = sdmmc_pins[3] >= 0 ? 4 : 1;
#ifdef CONFIG_SOC_SDMMC_USE_GPIO_MATRIX
    slot_config->clk = sdmmc_pins[0];
    slot_config->cmd = sdmmc_pins[1];
    slot_config->d0 = sdmmc_pins[2];
    slot_config->d1 = sdmmc_pins[3];
    slot_config->d2 = sdmmc_pins[4];
    slot_config->d3 = sdmmc_pins[5];
#endif
    slot_config->flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
}
#endif

#if SD_CARD_HAS_SDSPI
static void sd_card_make_spi_config(sdspi_device_config_t *device_config)
{
    *device_config = (sdspi_device_config_t)SDSPI_DEVICE_CONFIG_DEFAULT();
#if SOLAR_OS_BOARD_STORAGE_SDSPI
    device_config->host_id = solar_os_spi_bus_host();
    device_config->gpio_cs = SOLAR_OS_BOARD_PIN_SD_CARD_CS;
#else
    device_config->host_id = (spi_host_device_t)sdspi_runtime_host;
    device_config->gpio_cs = sdspi_runtime_cs;
#endif
    device_config->gpio_cd = SDSPI_SLOT_NO_CD;
    device_config->gpio_wp = SDSPI_SLOT_NO_WP;
    device_config->gpio_int = SDSPI_SLOT_NO_INT;
}
#endif

static void sd_card_reset_diagnostics(void)
{
    diagnostics_attempted = false;
    diagnostics_card_initialized = false;
    diagnostics_init_error = ESP_OK;
    diagnostics_mount_error = ESP_OK;
    diagnostics_diskio_error = ESP_OK;
    diagnostics_diskio_operation[0] = '\0';
    diagnostics_fresult = FR_OK;
}

esp_err_t sd_card_configure_sdspi(int host_id, int cs_pin)
{
#if SOLAR_OS_PACKAGE_EXPANSION_SDSPI && !SOLAR_OS_BOARD_HAS_SD
    if (host_id < 0 || cs_pin < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (card_ready || transport != SD_CARD_TRANSPORT_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    sdspi_runtime_host = host_id;
    sdspi_runtime_cs = cs_pin;
    sdspi_runtime_configured = true;
    transport = SD_CARD_TRANSPORT_SDSPI_RUNTIME;
    sd_card_reset_diagnostics();
    return ESP_OK;
#else
    (void)host_id;
    (void)cs_pin;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t sd_card_clear_sdspi_config(void)
{
#if SOLAR_OS_PACKAGE_EXPANSION_SDSPI && !SOLAR_OS_BOARD_HAS_SD
    if (transport != SD_CARD_TRANSPORT_SDSPI_RUNTIME) {
        return ESP_ERR_NOT_FOUND;
    }
    if (card_ready || sd_card_has_active_mounts()) {
        return ESP_ERR_INVALID_STATE;
    }
    sdspi_runtime_configured = false;
    sdspi_runtime_host = -1;
    sdspi_runtime_cs = -1;
    transport = SD_CARD_TRANSPORT_NONE;
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t sd_card_configure_sdmmc(int clk_pin,
                                  int cmd_pin,
                                  int d0_pin,
                                  int d1_pin,
                                  int d2_pin,
                                  int d3_pin)
{
#if SOLAR_OS_PACKAGE_EXPANSION_SDMMC && !SOLAR_OS_BOARD_STORAGE_SDSPI
    const bool four_bit = d1_pin >= 0 || d2_pin >= 0 || d3_pin >= 0;
    if (clk_pin < 0 || cmd_pin < 0 || d0_pin < 0 ||
        (four_bit && (d1_pin < 0 || d2_pin < 0 || d3_pin < 0))) {
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_IDF_TARGET_ESP32
    if (clk_pin != 14 || cmd_pin != 15 || d0_pin != 2 ||
        (four_bit && (d1_pin != 4 || d2_pin != 12 || d3_pin != 13))) {
        return ESP_ERR_NOT_SUPPORTED;
    }
#elif !defined(CONFIG_SOC_SDMMC_USE_GPIO_MATRIX)
    return ESP_ERR_NOT_SUPPORTED;
#endif
    if (card_ready || transport != SD_CARD_TRANSPORT_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    sdmmc_pins[0] = clk_pin;
    sdmmc_pins[1] = cmd_pin;
    sdmmc_pins[2] = d0_pin;
    sdmmc_pins[3] = d1_pin;
    sdmmc_pins[4] = d2_pin;
    sdmmc_pins[5] = d3_pin;
    transport = SD_CARD_TRANSPORT_SDMMC;
    sd_card_reset_diagnostics();
    return ESP_OK;
#else
    (void)clk_pin;
    (void)cmd_pin;
    (void)d0_pin;
    (void)d1_pin;
    (void)d2_pin;
    (void)d3_pin;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t sd_card_clear_sdmmc_config(void)
{
#if SOLAR_OS_PACKAGE_EXPANSION_SDMMC && !SOLAR_OS_BOARD_STORAGE_SDSPI
    if (transport != SD_CARD_TRANSPORT_SDMMC) {
        return ESP_ERR_NOT_FOUND;
    }
    if (card_ready || sd_card_has_active_mounts()) {
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < 6; i++) {
        sdmmc_pins[i] = -1;
    }
    transport = SD_CARD_TRANSPORT_NONE;
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool sd_card_configured(void)
{
    return transport != SD_CARD_TRANSPORT_NONE;
}

static esp_err_t sd_card_read_sector(uint64_t sector, uint8_t *buffer)
{
    if (!card_ready || card == NULL || buffer == NULL || sector > UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    return sdmmc_read_sectors(card, buffer, (uint32_t)sector, 1);
}

static uint64_t sd_card_capacity_sectors(void)
{
    if (card == NULL || card->csd.sector_size == 0) {
        return 0;
    }
    return card->csd.capacity;
}

static uint64_t sd_card_capacity_bytes(void)
{
    if (card == NULL) {
        return 0;
    }
    return (uint64_t)card->csd.capacity * (uint64_t)card->csd.sector_size;
}

static const char *mbr_type_name(uint8_t type)
{
    switch (type) {
    case 0x01:
        return "FAT12";
    case 0x04:
    case 0x06:
    case 0x0e:
        return "FAT16";
    case 0x0b:
    case 0x0c:
        return "FAT32";
    case 0x07:
        return "exFAT";
    case 0x0f:
    case 0x05:
        return "extended";
    case 0x83:
        return "Linux";
    case 0xee:
        return "GPT";
    case 0xef:
        return "EFI";
    default:
        return "part";
    }
}

static void detect_fs(uint64_t start_sector, char *fs, size_t fs_len)
{
    if (fs == NULL || fs_len == 0) {
        return;
    }
    fs[0] = '\0';

    /* SD sector transfers require an internal DMA-capable buffer. */
    uint8_t *sector = heap_caps_malloc(SD_CARD_SECTOR_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (sector == NULL) {
        return;
    }

    if (sd_card_read_sector(start_sector, sector) == ESP_OK &&
        sector[510] == 0x55 &&
        sector[511] == 0xaa) {
        if (memcmp(&sector[3], "EXFAT", 5) == 0) {
            strlcpy(fs, "exFAT", fs_len);
        } else if (memcmp(&sector[82], "FAT32", 5) == 0) {
            strlcpy(fs, "FAT32", fs_len);
        } else if (memcmp(&sector[54], "FAT", 3) == 0) {
            strlcpy(fs, "FAT", fs_len);
        }
    }

    heap_caps_free(sector);
}

static bool block_name_equal(const char *a, const char *b)
{
    return a != NULL && b != NULL && strcmp(a, b) == 0;
}

static sd_card_block_t *find_block(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < block_count; i++) {
        if (block_name_equal(blocks[i].name, name)) {
            return &blocks[i];
        }
    }
    return NULL;
}

static const sd_card_block_t *default_mount_block(void)
{
    for (size_t i = 1; i < block_count; i++) {
        if (blocks[i].mountable) {
            return &blocks[i];
        }
    }
    if (block_count > 0 && blocks[0].mountable) {
        return &blocks[0];
    }
    return block_count > 1 ? &blocks[1] : (block_count > 0 ? &blocks[0] : NULL);
}

static bool mount_point_in_use(const char *mount_point)
{
    for (size_t i = 0; i < SD_CARD_MAX_MOUNTS; i++) {
        if (mounts[i].active && strcmp(mounts[i].mount_point, mount_point) == 0) {
            return true;
        }
    }
    return false;
}

static sd_card_mount_t *find_mount_by_block(const char *name)
{
    for (size_t i = 0; i < SD_CARD_MAX_MOUNTS; i++) {
        if (mounts[i].active && strcmp(mounts[i].block_name, name) == 0) {
            return &mounts[i];
        }
    }
    return NULL;
}

static sd_card_mount_t *find_mount_by_target(const char *target)
{
    if (target == NULL || target[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < SD_CARD_MAX_MOUNTS; i++) {
        if (!mounts[i].active) {
            continue;
        }
        if (strcmp(mounts[i].block_name, target) == 0 ||
            strcmp(mounts[i].mount_point, target) == 0) {
            return &mounts[i];
        }
    }
    return NULL;
}

static sd_card_mount_t *alloc_mount(uint8_t *logical_volume)
{
    const uint8_t flash_volume = flash_storage_logical_volume();

    for (uint8_t vol = 0; vol < SD_CARD_MAX_MOUNTS; vol++) {
        if (vol == flash_volume) {
            continue;
        }

        bool used = false;
        for (size_t i = 0; i < SD_CARD_MAX_MOUNTS; i++) {
            if (mounts[i].active && mounts[i].logical_volume == vol) {
                used = true;
                break;
            }
        }
        if (used) {
            continue;
        }
        for (size_t i = 0; i < SD_CARD_MAX_MOUNTS; i++) {
            if (!mounts[i].active) {
                *logical_volume = vol;
                return &mounts[i];
            }
        }
    }
    return NULL;
}

static void update_block_mount_state(void)
{
    for (size_t i = 0; i < block_count; i++) {
        blocks[i].mounted = false;
        blocks[i].logical_volume = SD_CARD_LOGICAL_VOLUME_INVALID;
        blocks[i].mount_point[0] = '\0';
    }

    for (size_t i = 0; i < SD_CARD_MAX_MOUNTS; i++) {
        if (!mounts[i].active) {
            continue;
        }
        sd_card_block_t *block = find_block(mounts[i].block_name);
        if (block != NULL) {
            block->mounted = true;
            block->logical_volume = mounts[i].logical_volume;
            strlcpy(block->mount_point, mounts[i].mount_point, sizeof(block->mount_point));
        }
    }
}

static bool add_block(const sd_card_block_t *block)
{
    if (block == NULL || block_count >= SD_CARD_MAX_BLOCKS) {
        return false;
    }
    blocks[block_count++] = *block;
    return true;
}

static void scan_gpt(const uint8_t *mbr_sector)
{
    (void)mbr_sector;

    /* SD sector transfers require an internal DMA-capable buffer. */
    uint8_t *sector = heap_caps_malloc(SD_CARD_SECTOR_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (sector == NULL) {
        return;
    }

    if (sd_card_read_sector(1, sector) != ESP_OK || memcmp(sector, "EFI PART", 8) != 0) {
        heap_caps_free(sector);
        return;
    }

    const uint64_t entries_lba = get_u64le(&sector[72]);
    const uint32_t entry_count = get_u32le(&sector[80]);
    const uint32_t entry_size = get_u32le(&sector[84]);
    if (entry_size < 128 || entry_size > SD_CARD_SECTOR_BYTES) {
        heap_caps_free(sector);
        return;
    }

    const uint32_t max_entries = entry_count < (SD_CARD_MAX_BLOCKS - 1) ?
        entry_count :
        (SD_CARD_MAX_BLOCKS - 1);
    for (uint32_t i = 0; i < max_entries; i++) {
        const uint64_t sector_index = entries_lba + (((uint64_t)i * entry_size) / SD_CARD_SECTOR_BYTES);
        const uint32_t sector_offset = ((uint64_t)i * entry_size) % SD_CARD_SECTOR_BYTES;
        if (sector_offset + entry_size > SD_CARD_SECTOR_BYTES ||
            sd_card_read_sector(sector_index, sector) != ESP_OK) {
            break;
        }

        const uint8_t *entry = &sector[sector_offset];
        if (guid_is_zero(entry)) {
            continue;
        }

        const uint64_t first_lba = get_u64le(&entry[32]);
        const uint64_t last_lba = get_u64le(&entry[40]);
        if (last_lba < first_lba) {
            continue;
        }

        sd_card_block_t block = {
            .type = SD_CARD_BLOCK_PARTITION,
            .partition_number = (uint8_t)(i + 1),
            .start_sector = first_lba,
            .sector_count = last_lba - first_lba + 1,
            .sector_size = card->csd.sector_size,
            .mountable = false,
        };
        snprintf(block.name, sizeof(block.name), "sd0p%u", (unsigned)(i + 1));
        strlcpy(block.type_name, "GPT", sizeof(block.type_name));
        block.size_bytes = block.sector_count * (uint64_t)block.sector_size;
        detect_fs(block.start_sector, block.fs, sizeof(block.fs));
        add_block(&block);
    }

    heap_caps_free(sector);
}

static void scan_partitions(void)
{
    block_count = 0;
    memset(blocks, 0, sizeof(blocks));

    if (!card_ready || card == NULL) {
        return;
    }

    sd_card_block_t disk = {
        .type = SD_CARD_BLOCK_DISK,
        .sector_count = sd_card_capacity_sectors(),
        .sector_size = card->csd.sector_size,
        .size_bytes = sd_card_capacity_bytes(),
    };
    strlcpy(disk.name, "sd0", sizeof(disk.name));
    strlcpy(disk.type_name, "disk", sizeof(disk.type_name));
    add_block(&disk);

    /* SD sector transfers require an internal DMA-capable buffer. */
    uint8_t *sector = heap_caps_malloc(SD_CARD_SECTOR_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (sector == NULL) {
        update_block_mount_state();
        return;
    }

    bool has_mbr_parts = false;
    if (sd_card_read_sector(0, sector) == ESP_OK &&
        sector[510] == 0x55 &&
        sector[511] == 0xaa) {
        for (uint8_t i = 0; i < 4 && block_count < SD_CARD_MAX_BLOCKS; i++) {
            const uint8_t *entry = &sector[446 + (i * 16)];
            const uint8_t type = entry[4];
            const uint32_t start_lba = get_u32le(&entry[8]);
            const uint32_t sectors = get_u32le(&entry[12]);
            if (type == 0 || sectors == 0) {
                continue;
            }
            has_mbr_parts = true;
            if (type == 0xee) {
                scan_gpt(sector);
                break;
            }

            sd_card_block_t block = {
                .type = SD_CARD_BLOCK_PARTITION,
                .partition_number = (uint8_t)(i + 1),
                .mbr_type = type,
                .bootable = entry[0] == 0x80,
                .start_sector = start_lba,
                .sector_count = sectors,
                .sector_size = card->csd.sector_size,
                .mountable = type != 0x05 && type != 0x0f,
            };
            snprintf(block.name, sizeof(block.name), "sd0p%u", (unsigned)(i + 1));
            strlcpy(block.type_name, mbr_type_name(type), sizeof(block.type_name));
            block.size_bytes = block.sector_count * (uint64_t)block.sector_size;
            detect_fs(block.start_sector, block.fs, sizeof(block.fs));
            add_block(&block);
        }
    }

    if (!has_mbr_parts && block_count > 0) {
        detect_fs(0, blocks[0].fs, sizeof(blocks[0].fs));
        blocks[0].mountable = blocks[0].fs[0] != '\0';
        blocks[0].whole_disk_filesystem = blocks[0].mountable;
    }

    heap_caps_free(sector);
    update_block_mount_state();
}

static esp_err_t ensure_card_ready(void)
{
    if (card_ready) {
        return ESP_OK;
    }
    esp_err_t ret = ESP_OK;
    diagnostics_attempted = true;

#if SD_CARD_HAS_SDSPI
    if (transport == SD_CARD_TRANSPORT_SDSPI_BOARD ||
        transport == SD_CARD_TRANSPORT_SDSPI_RUNTIME) {
#if SOLAR_OS_PACKAGE_EXPANSION_SDSPI && !SOLAR_OS_BOARD_HAS_SD
        if (transport == SD_CARD_TRANSPORT_SDSPI_RUNTIME && !sdspi_runtime_configured) {
            diagnostics_init_error = ESP_ERR_NOT_SUPPORTED;
            set_mount_error_status(ESP_ERR_NOT_SUPPORTED);
            return ESP_ERR_NOT_SUPPORTED;
        }
#else
        ret = solar_os_spi_bus_acquire();
        if (ret != ESP_OK) {
            set_mount_error_status(ret);
            return ret;
        }
        sdspi_bus_acquired = true;
#endif

        host = (sdmmc_host_t)SDSPI_HOST_DEFAULT();
#if SOLAR_OS_BOARD_STORAGE_SDSPI
        host.slot = solar_os_spi_bus_host();
#else
        host.slot = sdspi_runtime_host;
#endif
        sdspi_device_config_t device_config;
        sd_card_make_spi_config(&device_config);

        ret = host.init();
        if (ret != ESP_OK) {
            diagnostics_init_error = ret;
            sd_card_deinit_host();
            set_mount_error_status(ret);
            return ret;
        }

        sdspi_dev_handle_t sdspi_handle = -1;
        ret = sdspi_host_init_device(&device_config, &sdspi_handle);
        if (ret != ESP_OK) {
            diagnostics_init_error = ret;
            sd_card_deinit_host();
            set_mount_error_status(ret);
            return ret;
        }
        host.slot = sdspi_handle;
        sdspi_device_ready = true;
    } else
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_SDMMC
    if (transport == SD_CARD_TRANSPORT_SDMMC) {
        host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
        sdmmc_slot_config_t slot_config;
        sd_card_make_slot_config(&slot_config);
        ret = host.init();
        if (ret != ESP_OK) {
            diagnostics_init_error = ret;
            set_mount_error_status(ret);
            return ret;
        }

        ret = sdmmc_host_init_slot(host.slot, &slot_config);
        if (ret != ESP_OK) {
            diagnostics_init_error = ret;
            sd_card_deinit_host();
            set_mount_error_status(ret);
            return ret;
        }
    } else
#endif
    {
        diagnostics_init_error = ESP_ERR_NOT_SUPPORTED;
        set_mount_error_status(ESP_ERR_NOT_SUPPORTED);
        return ESP_ERR_NOT_SUPPORTED;
    }

    memset(&card_storage, 0, sizeof(card_storage));
    ret = sdmmc_card_init(&host, &card_storage);
    if (ret != ESP_OK) {
        diagnostics_init_error = ret;
        sd_card_deinit_host();
        set_mount_error_status(ret);
        return ret;
    }
    diagnostics_card_initialized = true;

    BYTE pdrv = FF_DRV_NOT_USED;
    ret = ff_diskio_get_drive(&pdrv);
    if (ret != ESP_OK || pdrv == FF_DRV_NOT_USED) {
        diagnostics_init_error = ESP_ERR_NO_MEM;
        sd_card_deinit_host();
        set_mount_error_status(ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    card = &card_storage;
    physical_pdrv = pdrv;
#if SOLAR_OS_PACKAGE_EXPANSION_SDSPI && !SOLAR_OS_BOARD_HAS_SD
    if (transport == SD_CARD_TRANSPORT_SDSPI_RUNTIME) {
        ff_diskio_register(physical_pdrv, &sd_card_diskio);
    } else
#endif
    {
        ff_diskio_register_sdmmc(physical_pdrv, card);
        ff_sdmmc_set_disk_status_check(physical_pdrv, false);
    }
    diskio_registered = true;
    card_ready = true;
    scan_partitions();

    const uint64_t capacity_mb = sd_card_capacity_bytes() / (1024ULL * 1024ULL);
    snprintf(status_text, sizeof(status_text), "card %s %" PRIu64 "MB", card->cid.name, capacity_mb);
    ESP_LOGI(TAG, "SD card ready");
    sdmmc_card_print_info(stdout, card);
    return ESP_OK;
}

static esp_err_t unmount_one(sd_card_mount_t *mount)
{
    if (mount == NULL || !mount->active) {
        return ESP_ERR_INVALID_ARG;
    }

    char drv[3] = {(char)('0' + mount->logical_volume), ':', 0};
    FRESULT res = f_mount(NULL, drv, 0);
    esp_err_t err = res == FR_OK ? ESP_OK : ESP_FAIL;
    esp_err_t unregister_err = esp_vfs_fat_unregister_path(mount->mount_point);
    if (err == ESP_OK && unregister_err != ESP_OK) {
        err = unregister_err;
    }

    mount->active = false;
    mount->fs = NULL;
    update_block_mount_state();
    return err;
}

static bool sd_card_has_active_mounts(void)
{
    for (size_t i = 0; i < SD_CARD_MAX_MOUNTS; i++) {
        if (mounts[i].active) {
            return true;
        }
    }
    return false;
}

static void deinit_card_if_unused(void)
{
    if (sd_card_has_active_mounts()) {
        return;
    }

    if (diskio_registered && physical_pdrv != FF_DRV_NOT_USED) {
        ff_diskio_unregister(physical_pdrv);
    }
    diskio_registered = false;
    physical_pdrv = FF_DRV_NOT_USED;

    if (card_ready) {
        sd_card_deinit_host();
    }

    card_ready = false;
    card = NULL;
    block_count = 0;
    memset(blocks, 0, sizeof(blocks));
}

esp_err_t sd_card_mount_volume(const char *name, const char *mount_point)
{
    esp_err_t ret = ensure_card_ready();
    if (ret != ESP_OK) {
        return ret;
    }

    const sd_card_block_t *block = (name == NULL || name[0] == '\0') ?
        default_mount_block() :
        find_block(name);
    if (block == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!block->mountable) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    sd_card_mount_t *existing = find_mount_by_block(block->name);
    if (existing != NULL) {
        return ESP_OK;
    }

    char default_mount[SD_CARD_MOUNT_POINT_MAX];
    if (mount_point == NULL || mount_point[0] == '\0') {
        if (strcmp(block->name, "sd0") == 0 ||
            strcmp(block->name, "sd0p1") == 0 ||
            !sd_card_is_mounted()) {
            strlcpy(default_mount, SD_CARD_MOUNT_POINT, sizeof(default_mount));
        } else {
            snprintf(default_mount, sizeof(default_mount), "/mnt/%s", block->name);
        }
        mount_point = default_mount;
    }
    if (mount_point[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }
    if (mount_point_in_use(mount_point)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (strncmp(mount_point, "/mnt/", 5) == 0) {
        (void)mkdir("/mnt", 0777);
    }

    uint8_t logical_volume = 0;
    sd_card_mount_t *mount = alloc_mount(&logical_volume);
    if (mount == NULL) {
        return ESP_ERR_NO_MEM;
    }

#if FF_MULTI_PARTITION
    VolToPart[logical_volume].pd = physical_pdrv;
    VolToPart[logical_volume].pt = block->partition_number;
#endif

    char drv[3] = {(char)('0' + logical_volume), ':', 0};
    FATFS *fs = NULL;
    esp_vfs_fat_conf_t conf = {
        .base_path = mount_point,
        .fat_drive = drv,
        .max_files = strcmp(mount_point, SD_CARD_MOUNT_POINT) == 0 ?
            SD_CARD_DEFAULT_MAX_FILES :
            SD_CARD_EXTRA_MAX_FILES,
    };

    ret = esp_vfs_fat_register_cfg(&conf, &fs);
    if (ret != ESP_OK) {
        return ret;
    }

    FRESULT fresult = f_mount(fs, drv, 1);
    if (fresult != FR_OK) {
        diagnostics_fresult = fresult;
        esp_vfs_fat_unregister_path(mount_point);
        return ESP_FAIL;
    }

    memset(mount, 0, sizeof(*mount));
    mount->active = true;
    mount->logical_volume = logical_volume;
    mount->partition_number = block->partition_number;
    mount->fs = fs;
    strlcpy(mount->block_name, block->name, sizeof(mount->block_name));
    strlcpy(mount->mount_point, mount_point, sizeof(mount->mount_point));

    update_block_mount_state();
    snprintf(status_text, sizeof(status_text), "mounted %s at %s", block->name, mount_point);
    ESP_LOGI(TAG, "mounted %s at %s", block->name, mount_point);
    return ESP_OK;
}

esp_err_t sd_card_init(void)
{
    esp_err_t ret = sd_card_mount_volume(NULL, SD_CARD_MOUNT_POINT);
    diagnostics_mount_error = ret;
    if (ret != ESP_OK) {
        set_mount_error_status(ret);
        ESP_LOGW(TAG, "SD card mount failed: %s", esp_err_to_name(ret));

        const sd_card_block_t *block = default_mount_block();
        if (block != NULL && block->partition_number != 0) {
            ret = sd_card_mount_volume("sd0", SD_CARD_MOUNT_POINT);
            diagnostics_mount_error = ret;
            if (ret == ESP_OK) {
                return ret;
            }
        }
        return ret;
    }
    return ESP_OK;
}

esp_err_t sd_card_unmount_volume(const char *target)
{
    if (target == NULL || target[0] == '\0') {
        return sd_card_unmount();
    }

    sd_card_mount_t *mount = find_mount_by_target(target);
    if (mount == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = unmount_one(mount);
    if (ret == ESP_OK) {
        snprintf(status_text, sizeof(status_text), "unmounted %s", target);
    }
    deinit_card_if_unused();
    return ret;
}

esp_err_t sd_card_unmount(void)
{
    bool had_mount = false;
    esp_err_t ret = ESP_OK;

    for (int i = SD_CARD_MAX_MOUNTS - 1; i >= 0; i--) {
        if (!mounts[i].active) {
            continue;
        }
        had_mount = true;
        esp_err_t err = unmount_one(&mounts[i]);
        if (ret == ESP_OK && err != ESP_OK) {
            ret = err;
        }
    }

    deinit_card_if_unused();
    snprintf(status_text, sizeof(status_text), had_mount ? "unmounted" : "not mounted");
    return had_mount ? ret : ESP_ERR_INVALID_STATE;
}

esp_err_t sd_card_format(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ensure_card_ready();
    if (ret != ESP_OK) {
        return ret;
    }

    const sd_card_block_t *found = find_block(name);
    if (found == NULL) {
        deinit_card_if_unused();
        return ESP_ERR_NOT_FOUND;
    }
    const sd_card_block_t block = *found;
    if (block.type == SD_CARD_BLOCK_PARTITION && !block.mountable) {
        deinit_card_if_unused();
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (sd_card_has_active_mounts()) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t logical_volume = 0;
    if (alloc_mount(&logical_volume) == NULL) {
        deinit_card_if_unused();
        return ESP_ERR_NO_MEM;
    }

#if FF_MULTI_PARTITION
    VolToPart[logical_volume].pd = physical_pdrv;
    VolToPart[logical_volume].pt = block.partition_number;
#endif

    char drive[3] = {(char)('0' + logical_volume), ':', 0};
    const size_t work_size = 4096U;
    void *work = heap_caps_malloc(work_size,
                                  MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (work == NULL) {
        ret = ESP_ERR_NO_MEM;
    } else {
        const MKFS_PARM options = {
            .fmt = (BYTE)(FM_ANY |
                          (block.partition_number == 0 ? FM_SFD : 0)),
            .n_fat = 1,
            .align = 0,
            .n_root = 0,
            .au_size = 0,
        };
        ret = f_mkfs(drive, &options, work, work_size) == FR_OK ?
            ESP_OK : ESP_FAIL;
        heap_caps_free(work);
    }

    deinit_card_if_unused();
    if (ret == ESP_OK) {
        snprintf(status_text, sizeof(status_text), "formatted %s", name);
    } else {
        set_mount_error_status(ret);
    }
    return ret;
}

bool sd_card_is_mounted(void)
{
    for (size_t i = 0; i < SD_CARD_MAX_MOUNTS; i++) {
        if (mounts[i].active && strcmp(mounts[i].mount_point, SD_CARD_MOUNT_POINT) == 0) {
            return true;
        }
    }
    return false;
}

bool sd_card_has_mounts(void)
{
    return sd_card_has_active_mounts();
}

void sd_card_get_status(char *buffer, size_t len)
{
    if (len == 0) {
        return;
    }

    strlcpy(buffer, status_text, len);
}

const char *sd_card_mount_point(void)
{
    return SD_CARD_MOUNT_POINT;
}

esp_err_t sd_card_rescan(void)
{
    esp_err_t ret = ensure_card_ready();
    if (ret != ESP_OK) {
        return ret;
    }
    scan_partitions();
    return ESP_OK;
}

size_t sd_card_block_count(void)
{
    return block_count;
}

bool sd_card_get_block(size_t index, sd_card_block_t *block)
{
    if (block == NULL || index >= block_count) {
        return false;
    }
    *block = blocks[index];
    return true;
}

bool sd_card_get_last_diagnostics(sd_card_diagnostics_t *diagnostics)
{
    if (diagnostics == NULL || !diagnostics_attempted) {
        return false;
    }
    memset(diagnostics, 0, sizeof(*diagnostics));
    diagnostics->attempted = true;
    diagnostics->card_initialized = diagnostics_card_initialized;
    diagnostics->init_error = diagnostics_init_error;
    diagnostics->mount_error = diagnostics_mount_error;
    diagnostics->diskio_error = diagnostics_diskio_error;
    diagnostics->fatfs_result = (int)diagnostics_fresult;
    strlcpy(diagnostics->diskio_operation,
            diagnostics_diskio_operation,
            sizeof(diagnostics->diskio_operation));
    if (!diagnostics_card_initialized) {
        return true;
    }

    const sdmmc_card_t *info = &card_storage;
    if (info->is_sdio) {
        diagnostics->kind = SD_CARD_KIND_SDIO;
    } else if (info->is_mmc) {
        diagnostics->kind = SD_CARD_KIND_MMC;
    } else if ((info->ocr & SD_OCR_SDHC_CAP) == 0) {
        diagnostics->kind = SD_CARD_KIND_SDSC;
    } else if ((info->ocr & SD_OCR_S18_RA) != 0) {
        diagnostics->kind = SD_CARD_KIND_SDHC_UHS1;
    } else {
        diagnostics->kind = SD_CARD_KIND_SDHC;
    }
    diagnostics->ddr = info->is_ddr;
    strlcpy(diagnostics->name, info->cid.name, sizeof(diagnostics->name));
    diagnostics->real_freq_khz = info->real_freq_khz;
    diagnostics->max_freq_khz = info->max_freq_khz;
    diagnostics->size_bytes = (uint64_t)info->csd.capacity * info->csd.sector_size;
    diagnostics->csd_version = info->is_mmc ? info->csd.csd_ver : info->csd.csd_ver + 1;
    diagnostics->sector_size = info->csd.sector_size;
    diagnostics->capacity_sectors = info->csd.capacity;
    diagnostics->read_block_len = info->csd.read_block_len;
    diagnostics->bus_width = info->is_mmc ?
        (uint32_t)(1U << info->log_bus_width) :
        (info->ssr.cur_bus_width ? 4U : 1U);
    return true;
}
