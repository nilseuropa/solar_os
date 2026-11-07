#include "solar_os_disk_image.h"
#include "solar_os_board_storage.h"
#include "solar_os_ramfs.h"
#include "diskio_impl.h"
#include "esp_vfs_fat.h"
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/lock.h>
#include <sys/stat.h>
#include <unistd.h>

static const ff_diskio_impl_t *impls[FF_VOLUMES];
PARTITION VolToPart[FF_VOLUMES];
static FATFS *filesystems[FF_VOLUMES];
static char mount_paths[FF_VOLUMES][32];
static bool fail_sync, fail_vfs;
int __real_fsync(int fd);
int __wrap_fsync(int fd) { return fail_sync ? -1 : __real_fsync(fd); }

static pthread_mutex_t lock_init = PTHREAD_MUTEX_INITIALIZER;
static void take_lock(_lock_t *lock, bool recursive)
{
    pthread_mutex_lock(&lock_init);
    if (!*lock) {
        *lock = malloc(sizeof(**lock)); assert(*lock);
        pthread_mutexattr_t attr; pthread_mutexattr_init(&attr);
        if (recursive) pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(*lock, &attr); pthread_mutexattr_destroy(&attr);
    }
    pthread_mutex_unlock(&lock_init);
    pthread_mutex_lock(*lock);
}
void _lock_acquire(_lock_t *lock) { take_lock(lock, false); }
void _lock_release(_lock_t *lock) { pthread_mutex_unlock(*lock); }
void _lock_acquire_recursive(_lock_recursive_t *lock) { take_lock(lock, true); }
void _lock_release_recursive(_lock_recursive_t *lock) { pthread_mutex_unlock(*lock); }
static pthread_mutex_t fat_locks[FF_VOLUMES + 1];
int ff_mutex_create(int vol) { return !pthread_mutex_init(&fat_locks[vol], NULL); }
void ff_mutex_delete(int vol) { pthread_mutex_destroy(&fat_locks[vol]); }
int ff_mutex_take(int vol) { return !pthread_mutex_lock(&fat_locks[vol]); }
void ff_mutex_give(int vol) { pthread_mutex_unlock(&fat_locks[vol]); }
void *ff_memalloc(unsigned size) { return malloc(size); }
void ff_memfree(void *p) { free(p); }
DWORD get_fattime(void) { return 0; }
void vTaskDelay(unsigned ticks) { (void)ticks; }

esp_err_t ff_diskio_get_drive(BYTE *drive)
{
    for (BYTE i = 0; i < FF_VOLUMES; ++i) if (!impls[i]) { *drive = i; return ESP_OK; }
    return ESP_ERR_NOT_FOUND;
}
void ff_diskio_register(BYTE drive, const ff_diskio_impl_t *impl) { impls[drive] = impl; }
DSTATUS disk_initialize(BYTE drive) { return impls[drive]->init(drive); }
DSTATUS disk_status(BYTE drive) { return impls[drive]->status(drive); }
DRESULT disk_read(BYTE drive, BYTE *data, LBA_t sector, UINT count) { return impls[drive]->read(drive, data, sector, count); }
DRESULT disk_write(BYTE drive, const BYTE *data, LBA_t sector, UINT count) { return impls[drive]->write(drive, data, sector, count); }
DRESULT disk_ioctl(BYTE drive, BYTE cmd, void *data) { return impls[drive]->ioctl(drive, cmd, data); }
esp_err_t esp_vfs_fat_register_cfg(const esp_vfs_fat_conf_t *conf, FATFS **fs)
{
    if (fail_vfs) return ESP_FAIL;
    unsigned vol = conf->fat_drive[0] - '0'; assert(vol < FF_VOLUMES && !filesystems[vol]);
    *fs = filesystems[vol] = calloc(1, sizeof(FATFS)); assert(*fs);
    strlcpy(mount_paths[vol], conf->base_path, sizeof(mount_paths[vol]));
    return ESP_OK;
}
esp_err_t esp_vfs_fat_unregister_path(const char *path)
{
    for (unsigned i = 0; i < FF_VOLUMES; ++i) if (!strcmp(path, mount_paths[i])) {
        free(filesystems[i]); filesystems[i] = NULL; mount_paths[i][0] = 0; return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
size_t strlcpy(char *out, const char *in, size_t size)
{ size_t n = strlen(in); if (size) { size_t c = n < size ? n : size - 1; memcpy(out, in, c); out[c] = 0; } return n; }
bool solar_os_board_storage_available(void) { return true; }
size_t solar_os_board_storage_block_count(void) { return 1; }
bool solar_os_board_storage_get_block(size_t index, solar_os_board_storage_block_t *block)
{
    if (index) return false;
    memset(block, 0, sizeof(*block));
    strcpy(block->name, "sd0p1"); strcpy(block->mount_point, "/tmp");
    block->mounted = true; block->mountable = true; block->logical_volume = 0; return true;
}
bool solar_os_board_storage_is_mounted(void) { return true; }
const char *solar_os_board_storage_mount_point(void) { return "/tmp"; }
esp_err_t solar_os_board_storage_unmount(void) { return ESP_OK; }
esp_err_t solar_os_board_storage_unmount_volume(const char *p) { (void)p; return ESP_OK; }
bool flash_storage_is_mounted(void) { return true; }
bool flash_storage_is_unrecognized(void) { return false; }
const char *flash_storage_mount_point(void) { return "/flash"; }
uint8_t flash_storage_logical_volume(void) { return 1; }
uint64_t flash_storage_size_bytes(void) { return 1024 * 1024; }
esp_err_t flash_storage_unmount(void) { return ESP_OK; }
esp_err_t flash_storage_get_usage(uint64_t *a, uint64_t *b, uint64_t *c) { *a = *b = *c = 0; return ESP_OK; }
size_t solar_os_ramfs_mount_count(void) { return 0; }
bool solar_os_ramfs_get_info(size_t index, solar_os_ramfs_info_t *info) { return false; }
bool solar_os_ramfs_path_has_mount_prefix(const char *path) { return false; }
esp_err_t solar_os_ramfs_path_mount_point(const char *p, char *out, size_t n) { return ESP_ERR_NOT_FOUND; }
esp_err_t solar_os_ramfs_get_usage_for_path(const char *p, uint64_t *a, uint64_t *b, uint64_t *c) { return ESP_ERR_NOT_FOUND; }

static BYTE mounted_volume(void)
{ solar_os_storage_block_t block; assert(solar_os_disk_image_get_block(0, &block)); return block.logical_volume; }
static void host_write_file(const char *contents)
{
    char path[] = "0:/HELLO.TXT"; path[0] += mounted_volume();
    FIL file; UINT count;
    assert(f_open(&file, path, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK);
    assert(f_write(&file, contents, strlen(contents), &count) == FR_OK && count == strlen(contents));
    assert(f_close(&file) == FR_OK);
}
static void host_read_file(const char *expected)
{
    char path[] = "0:/HELLO.TXT"; path[0] += mounted_volume();
    FIL file; UINT count; char contents[32] = {0};
    assert(f_open(&file, path, FA_READ) == FR_OK);
    assert(f_read(&file, contents, sizeof(contents) - 1, &count) == FR_OK);
    assert(!strcmp(contents, expected)); assert(f_close(&file) == FR_OK);
}

static int format_fd;
static DSTATUS format_status(BYTE drive) { return 0; }
static DRESULT format_read(BYTE drive, BYTE *data, uint32_t sector, unsigned count)
{ return pread(format_fd, data, count * 512U, (off_t)sector * 512U) == count * 512U ? RES_OK : RES_ERROR; }
static DRESULT format_write(BYTE drive, const BYTE *data, uint32_t sector, unsigned count)
{ return pwrite(format_fd, data, count * 512U, (off_t)sector * 512U) == count * 512U ? RES_OK : RES_ERROR; }
static DRESULT format_ioctl(BYTE drive, BYTE command, void *data)
{
    if (command == CTRL_SYNC) return RES_OK;
    if (command == GET_SECTOR_COUNT) { struct stat st; assert(!fstat(format_fd, &st)); *(LBA_t *)data = st.st_size / 512; }
    else if (command == GET_SECTOR_SIZE) *(WORD *)data = 512;
    else if (command == GET_BLOCK_SIZE) *(DWORD *)data = 1;
    else return RES_PARERR;
    return RES_OK;
}
static void check_fat_variant(const char *path, uint32_t bytes, BYTE format, unsigned partition, BYTE expected_type)
{
    format_fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600); assert(format_fd >= 0);
    assert(!ftruncate(format_fd, bytes));
    static const ff_diskio_impl_t formatter = {.init = format_status, .status = format_status,
        .read = format_read, .write = format_write, .ioctl = format_ioctl};
    ff_diskio_register(2, &formatter); VolToPart[2].pd = 2; VolToPart[2].pt = partition;
    uint8_t work[8192];
    if (partition) { const LBA_t parts[4] = {50, 50, 0, 0}; assert(f_fdisk(2, parts, work) == FR_OK); }
    MKFS_PARM options = {.fmt = format, .n_fat = 2, .au_size = 512};
    assert(f_mkfs("2:", &options, work, sizeof(work)) == FR_OK);
    ff_diskio_unregister(2); assert(!close(format_fd));
    assert(solar_os_disk_image_mount(path, "/mnt/dos", partition) == ESP_OK);
    assert(filesystems[mounted_volume()]->fs_type == expected_type);
    host_write_file("FAT variant edit"); assert(solar_os_disk_image_unmount("/mnt/dos") == ESP_OK);
    assert(solar_os_disk_image_mount(path, "/mnt/dos", 0) == ESP_OK);
    host_read_file("FAT variant edit"); assert(solar_os_disk_image_unmount("/mnt/dos") == ESP_OK);
    if (partition == 2) assert(solar_os_disk_image_mount(path, "/mnt/dos", 1) == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_storage_remove(path) == ESP_OK);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    char path[160], blank[160]; snprintf(path, sizeof(path), "%s/dos.img", argv[1]);
    snprintf(blank, sizeof(blank), "%s/blank.img", argv[1]);
    static const ff_diskio_impl_t reserved = {0}; impls[0] = impls[1] = &reserved;
    assert(solar_os_disk_image_create(path, 16) == ESP_OK);
    assert(solar_os_disk_image_create(path, 16) == ESP_ERR_INVALID_STATE);
    assert(solar_os_disk_image_create(blank, 0) == ESP_ERR_INVALID_SIZE);
    assert(solar_os_disk_image_create(blank, 2048) == ESP_ERR_INVALID_SIZE);
    assert(solar_os_disk_image_mount(path, "/mnt/dos", 0) == ESP_OK);
    assert(mounted_volume() == 2 && solar_os_storage_mount_count() == 3);
    assert(solar_os_storage_block_count() == 3);
    char resolved[160]; assert(solar_os_storage_resolve_path("/mnt/dos/HELLO.TXT", resolved, sizeof(resolved)) == ESP_OK);
    assert(!strcmp(resolved, "/mnt/dos/HELLO.TXT"));
    solar_os_storage_usage_t usage;
    assert(solar_os_storage_get_usage_for_path(resolved, &usage) == ESP_OK && usage.total_bytes > 15 * 1024 * 1024);
    host_write_file("hello from SolarOS");
    static const char guest, other;
    uint32_t handle = 0, size;
    assert(solar_os_disk_image_open(path, &guest, &handle, &size) == ESP_ERR_INVALID_STATE && !handle);
    assert(solar_os_storage_remove(path) == ESP_ERR_INVALID_STATE);
    assert(solar_os_storage_rename(path, blank) == ESP_ERR_INVALID_STATE);
    assert(solar_os_storage_write_file(path, "X", 1, false) == ESP_ERR_INVALID_STATE);
    assert(solar_os_storage_unmount() == ESP_ERR_INVALID_STATE);
    assert(solar_os_storage_unmount_volume(NULL) == ESP_ERR_INVALID_STATE);
    assert(solar_os_storage_unmount_volume("sd0p1") == ESP_ERR_INVALID_STATE);
    assert(solar_os_disk_image_mount(path, "/tmp", 0) == ESP_ERR_INVALID_STATE);
    assert(solar_os_disk_image_create("/mnt/dos/nested.img", 1) == ESP_ERR_INVALID_STATE);
    fail_sync = true; assert(solar_os_disk_image_unmount("/mnt/dos") == ESP_FAIL);
    assert(solar_os_disk_image_mount_count() == 1); fail_sync = false;
    assert(solar_os_storage_unmount_volume("/mnt/dos") == ESP_OK);
    assert(solar_os_disk_image_open(path, &guest, &handle, &size) == ESP_OK && size == 16 * 1024 * 1024);
    assert(link(path, blank) == 0);
    uint32_t alias_handle;
    assert(solar_os_disk_image_open(blank, &other, &alias_handle, &size) == ESP_ERR_INVALID_STATE);
    assert(solar_os_storage_remove(blank) == ESP_ERR_INVALID_STATE);
    assert(unlink(blank) == 0);
    assert(solar_os_disk_image_mount(path, "/mnt/dos", 1) == ESP_ERR_INVALID_STATE);
    uint32_t duplicate;
    assert(solar_os_disk_image_open(path, &other, &duplicate, &size) == ESP_ERR_INVALID_STATE);
    uint8_t sector[512];
    assert(solar_os_disk_image_read(handle, &other, 0, sector, sizeof(sector)) == ESP_ERR_INVALID_STATE);
    assert(solar_os_disk_image_read(handle, &guest, 0, sector, sizeof(sector)) == ESP_OK);
    assert(sector[510] == 0x55 && sector[511] == 0xaa);
    uint32_t first_sector = (uint32_t)sector[454] | (uint32_t)sector[455] << 8 |
        (uint32_t)sector[456] << 16 | (uint32_t)sector[457] << 24;
    assert(sector[446] == 0x80 && first_sector == 63);
    assert(solar_os_disk_image_read(handle, &guest, first_sector * 512U, sector, sizeof(sector)) == ESP_OK);
    assert(sector[24] == 63 && sector[25] == 0 && sector[26] == 16 && sector[27] == 0 && sector[36] == 0x80);
    assert(solar_os_disk_image_write(handle, &guest, size - 1, sector, 2) == ESP_ERR_INVALID_SIZE);
    assert(solar_os_disk_image_read(handle, &guest, UINT32_MAX, sector, 1) == ESP_ERR_INVALID_SIZE);
    assert(solar_os_disk_image_read(handle, &guest, 0, sector, 65537) == ESP_ERR_INVALID_SIZE);
    memset(sector, 0x5a, sizeof(sector));
    assert(solar_os_disk_image_write(handle, &guest, size - 512, sector, sizeof(sector)) == ESP_OK);
    memset(sector, 0, sizeof(sector));
    assert(solar_os_disk_image_read(handle, &guest, size - 512, sector, sizeof(sector)) == ESP_OK && sector[0] == 0x5a);
    fail_sync = true; assert(solar_os_disk_image_sync(handle, &guest) == ESP_FAIL); fail_sync = false;
    assert(solar_os_disk_image_close_owner(&guest) == ESP_OK);
    assert(solar_os_disk_image_read(handle, &guest, 0, sector, 1) == ESP_ERR_INVALID_STATE);
    assert(solar_os_disk_image_mount(path, "/mnt/dos", 1) == ESP_OK);
    host_read_file("hello from SolarOS"); host_write_file("second host edit");
    assert(solar_os_disk_image_unmount("img0") == ESP_OK);
    assert(solar_os_disk_image_mount(path, "/mnt/dos", 0) == ESP_OK);
    host_read_file("second host edit"); assert(solar_os_disk_image_unmount(path) == ESP_OK);
    fail_vfs = true; assert(solar_os_disk_image_mount(path, "/mnt/dos", 0) == ESP_FAIL);
    fail_vfs = false; assert(!solar_os_disk_image_path_busy(path) && !impls[2]);
    int fd = open(blank, O_RDWR | O_CREAT | O_EXCL, 0600); assert(fd >= 0);
    assert(ftruncate(fd, 1024 * 1024) == 0); close(fd);
    assert(solar_os_disk_image_mount(blank, "/mnt/dos", 0) == ESP_ERR_NOT_SUPPORTED);
    assert(!solar_os_disk_image_path_busy(blank) && !impls[2]);
    assert(solar_os_disk_image_mount(path, "/mnt/dos", 5) == ESP_ERR_INVALID_ARG);
    assert(solar_os_storage_remove(path) == ESP_OK); assert(solar_os_storage_remove(blank) == ESP_OK);
    check_fat_variant(blank, 1440U * 1024U, FM_FAT | FM_SFD, 0, FS_FAT12);
    check_fat_variant(blank, 64U * 1024U * 1024U, FM_FAT32 | FM_SFD, 0, FS_FAT32);
    check_fat_variant(blank, 16U * 1024U * 1024U, FM_FAT, 2, FS_FAT16);
    puts("Real FatFS image creation, host edits, claims, bounds, sync and cleanup passed");
    return 0;
}
