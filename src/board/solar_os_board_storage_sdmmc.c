#include "solar_os_board_storage.h"

#include <string.h>

#include "sd_card.h"

static void storage_block_from_driver(solar_os_board_storage_block_t *out,
                                      const sd_card_block_t *in)
{
    memset(out, 0, sizeof(*out));
    strlcpy(out->name, in->name, sizeof(out->name));
    out->type = in->type == SD_CARD_BLOCK_PARTITION ?
        SOLAR_OS_BOARD_STORAGE_BLOCK_PARTITION :
        SOLAR_OS_BOARD_STORAGE_BLOCK_DISK;
    out->partition_number = in->partition_number;
    out->mbr_type = in->mbr_type;
    out->bootable = in->bootable;
    out->mountable = in->mountable;
    out->mounted = in->mounted;
    out->whole_disk_filesystem = in->whole_disk_filesystem;
    out->logical_volume = in->logical_volume;
    out->start_sector = in->start_sector;
    out->sector_count = in->sector_count;
    out->sector_size = in->sector_size;
    out->size_bytes = in->size_bytes;
    strlcpy(out->fs, in->fs, sizeof(out->fs));
    strlcpy(out->type_name, in->type_name, sizeof(out->type_name));
    strlcpy(out->mount_point, in->mount_point, sizeof(out->mount_point));
}

esp_err_t solar_os_board_storage_init(void)
{
    return sd_card_init();
}

esp_err_t solar_os_board_storage_mount(void)
{
    return sd_card_init();
}

esp_err_t solar_os_board_storage_mount_volume(const char *name, const char *mount_point)
{
    return sd_card_mount_volume(name, mount_point);
}

esp_err_t solar_os_board_storage_unmount(void)
{
    return sd_card_unmount();
}

esp_err_t solar_os_board_storage_unmount_volume(const char *target)
{
    return sd_card_unmount_volume(target);
}

bool solar_os_board_storage_is_mounted(void)
{
    return sd_card_is_mounted();
}

void solar_os_board_storage_get_status(char *buffer, size_t len)
{
    sd_card_get_status(buffer, len);
}

const char *solar_os_board_storage_mount_point(void)
{
    return sd_card_mount_point();
}

esp_err_t solar_os_board_storage_rescan(void)
{
    return sd_card_rescan();
}

size_t solar_os_board_storage_block_count(void)
{
    return sd_card_block_count();
}

bool solar_os_board_storage_get_block(size_t index, solar_os_board_storage_block_t *block)
{
    if (block == NULL) {
        return false;
    }

    sd_card_block_t driver_block;
    if (!sd_card_get_block(index, &driver_block)) {
        return false;
    }

    storage_block_from_driver(block, &driver_block);
    return true;
}

