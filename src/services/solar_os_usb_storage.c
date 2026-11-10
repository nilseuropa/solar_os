#include "solar_os_usb_storage.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/lock.h>
#include <sys/stat.h>
#include <time.h>
#include "diskio_impl.h"
#include "esp_timer.h"
#include "esp_vfs.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_disk_image.h"
#include "solar_os_memory.h"
#include "solar_os_task.h"
#include "usb_msc_port.h"

#define USB_FILES_MAX 8
#define USB_POLL_MS 20

typedef struct {
    FIL file;
    uint32_t generation;
    WORD date, time;
    BYTE attributes;
    bool append;
} usb_file_t;

typedef struct {
    DIR base;
    FF_DIR directory;
    struct dirent entry;
    uint32_t generation;
    long position;
} usb_dir_t;

static _lock_t guard;
static FATFS *filesystem;
static usb_file_t *files[USB_FILES_MAX];
static solar_os_usb_msc_info_t geometry;
static uint32_t generation;
static size_t open_handles;
static BYTE volume = UINT8_MAX;
static uint8_t address;
static bool started, enabled, vfs_registered;
static bool ejected;
static int64_t last_media_probe;
static esp_err_t last_error;
static atomic_uint pending_address;
static atomic_bool removed, stop_worker, worker_done = true;

static void lock(void) { _lock_acquire_recursive(&guard); }
static void unlock(void) { _lock_release_recursive(&guard); }

static bool available(void)
{ return filesystem && !atomic_load(&removed) && solar_os_usb_msc_media_current(&geometry); }

static int fat_error(FRESULT result)
{
    switch (result) {
    case FR_OK: return 0;
    case FR_NO_FILE: case FR_NO_PATH: errno = ENOENT; break;
    case FR_EXIST: errno = EEXIST; break;
    case FR_DENIED: errno = EACCES; break;
    case FR_WRITE_PROTECTED: errno = EROFS; break;
    case FR_INVALID_NAME: case FR_INVALID_PARAMETER: errno = EINVAL; break;
    case FR_NOT_ENOUGH_CORE: errno = ENOMEM; break;
    case FR_TOO_MANY_OPEN_FILES: errno = EMFILE; break;
    case FR_LOCKED: errno = EBUSY; break;
    default: errno = EIO; break;
    }
    return -1;
}

/* A permanent VFS reservation prevents absent USB paths reaching a root
 * filesystem. Generation checks keep stale FILE/DIR handles from touching
 * either freed FatFS state or a newly inserted drive. All FatFS calls and
 * detach operations are serialized; USB callbacks never take this lock. */
static bool fat_path(const char *name, char *path, size_t size)
{
    if (!available()) { errno = ENODEV; return false; }
    if (!name || strchr(name, ':') || strstr(name, "\\")) { errno = EINVAL; return false; }
    int n = snprintf(path, size, "%u:/%s", volume, name[0] == '/' ? name + 1 : name);
    if (n < 0 || (size_t)n >= size) { errno = ENAMETOOLONG; return false; }
    return true;
}

static usb_file_t *get_file(int fd)
{
    if (fd < 0 || fd >= USB_FILES_MAX || !files[fd]) { errno = EBADF; return NULL; }
    if (!available() || files[fd]->generation != generation) { errno = ENODEV; return NULL; }
    return files[fd];
}

static int usb_open(void *ctx, const char *name, int flags, int mode)
{
    (void)ctx; (void)mode;
    lock();
    int fd = -1;
    char path[SOLAR_OS_STORAGE_PATH_MAX + 4];
    if (!fat_path(name, path, sizeof(path))) goto done;
    for (int i = 0; i < USB_FILES_MAX; ++i) if (!files[i]) { fd = i; break; }
    if (fd < 0) { errno = EMFILE; goto done; }
    usb_file_t *file = solar_os_memory_calloc(1, sizeof(*file), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "usb.storage.file");
    if (!file) { fd = -1; errno = ENOMEM; goto done; }
    BYTE access = (flags & O_ACCMODE) == O_RDONLY ? FA_READ :
        (flags & O_ACCMODE) == O_WRONLY ? FA_WRITE : FA_READ | FA_WRITE;
    access |= (flags & O_CREAT) ? ((flags & O_EXCL) ? FA_CREATE_NEW :
        (flags & O_TRUNC) ? FA_CREATE_ALWAYS : FA_OPEN_ALWAYS) : FA_OPEN_EXISTING;
    FRESULT result = f_open(&file->file, path, access);
    if (result == FR_OK && (flags & O_TRUNC) && !(flags & O_CREAT)) result = f_truncate(&file->file);
    if (result != FR_OK) {
        if (file->file.obj.fs) (void)f_close(&file->file);
        solar_os_memory_free(file); fd = fat_error(result); goto done;
    }
    file->generation = generation; file->append = (flags & O_APPEND) != 0;
    FILINFO info;
    if (f_stat(path, &info) == FR_OK) {
        file->date = info.fdate; file->time = info.ftime; file->attributes = info.fattrib;
    }
    files[fd] = file; open_handles++;
done:
    unlock(); return fd;
}

static int usb_close(void *ctx, int fd)
{
    (void)ctx;
    lock();
    int result = -1;
    if (fd < 0 || fd >= USB_FILES_MAX || !files[fd]) { errno = EBADF; goto done; }
    usb_file_t *file = files[fd];
    bool current = file->generation == generation;
    result = available() && current ? fat_error(f_close(&file->file)) : (errno = ENODEV, -1);
    if (current) open_handles--;
    files[fd] = NULL; solar_os_memory_free(file);
done:
    unlock(); return result;
}

static ssize_t usb_read(void *ctx, int fd, void *data, size_t size)
{
    (void)ctx;
    lock();
    usb_file_t *file = get_file(fd);
    UINT count = 0;
    ssize_t result = file ? fat_error(f_read(&file->file, data,
        size > INT_MAX ? INT_MAX : (UINT)size, &count)) : -1;
    if (result == 0) result = count;
    unlock(); return result;
}

static ssize_t usb_write(void *ctx, int fd, const void *data, size_t size)
{
    (void)ctx;
    lock();
    usb_file_t *file = get_file(fd);
    UINT count = 0;
    FRESULT result = FR_OK;
    if (file && file->append) result = f_lseek(&file->file, f_size(&file->file));
    if (file && result == FR_OK) result = f_write(&file->file, data,
        size > INT_MAX ? INT_MAX : (UINT)size, &count);
    ssize_t written = file ? fat_error(result) : -1;
    if (written == 0) {
        written = count;
        if (!count && size) { errno = ENOSPC; written = -1; }
        if (count) { DWORD timestamp = get_fattime(); file->date = timestamp >> 16; file->time = timestamp; }
    }
    unlock(); return written;
}

static off_t usb_lseek(void *ctx, int fd, off_t offset, int whence)
{
    (void)ctx;
    lock();
    usb_file_t *file = get_file(fd);
    off_t result = -1;
    if (file) {
        int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)f_tell(&file->file) :
            whence == SEEK_END ? (int64_t)f_size(&file->file) : -1;
        int64_t position = base + (int64_t)offset;
        if (base < 0 || position < 0 || position > UINT32_MAX || position > LONG_MAX) errno = EINVAL;
        else if (fat_error(f_lseek(&file->file, (FSIZE_t)position)) == 0) result = f_tell(&file->file);
    }
    unlock(); return result;
}

static int usb_fsync(void *ctx, int fd)
{
    (void)ctx;
    lock(); usb_file_t *file = get_file(fd);
    int result = file ? fat_error(f_sync(&file->file)) : -1;
    unlock(); return result;
}

static void fill_stat(struct stat *st, uint64_t size, BYTE attributes, WORD date, WORD time_value)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = ((attributes & AM_DIR) ? S_IFDIR : S_IFREG) | 0444;
    if (!(attributes & AM_RDO)) st->st_mode |= 0222;
    if (attributes & AM_DIR) st->st_mode |= 0111;
    st->st_size = size; st->st_blksize = geometry.sector_size;
    st->st_nlink = 1;
    if (!date) return;
    struct tm tm = {.tm_year = (date >> 9) + 80, .tm_mon = ((date >> 5) & 15) - 1,
        .tm_mday = date & 31, .tm_hour = time_value >> 11,
        .tm_min = (time_value >> 5) & 63, .tm_sec = (time_value & 31) * 2, .tm_isdst = -1};
    st->st_mtime = st->st_atime = st->st_ctime = mktime(&tm);
}

static int usb_fstat(void *ctx, int fd, struct stat *st)
{
    (void)ctx;
    lock(); usb_file_t *file = get_file(fd);
    if (file) {
        fill_stat(st, f_size(&file->file), file->attributes, file->date, file->time);
        /* Give image backing-file ownership a stable identity without raw
         * access to this service's private FatFS logical drive. */
        st->st_ino = file->file.obj.sclust; st->st_dev = generation;
    }
    unlock(); return file ? 0 : -1;
}

static int usb_stat(void *ctx, const char *name, struct stat *st)
{
    (void)ctx;
    lock();
    char path[SOLAR_OS_STORAGE_PATH_MAX + 4];
    FILINFO info;
    int result = -1;
    if (fat_path(name, path, sizeof(path))) {
        if (!name[0] || !strcmp(name, "/")) { fill_stat(st, 0, AM_DIR, 0, 0); result = 0; }
        else if ((result = fat_error(f_stat(path, &info))) == 0)
            fill_stat(st, info.fsize, info.fattrib, info.fdate, info.ftime);
    }
    unlock(); return result;
}

static int usb_path_operation(const char *name, bool directory, bool remove)
{
    lock();
    char path[SOLAR_OS_STORAGE_PATH_MAX + 4];
    int result = -1;
    if (fat_path(name, path, sizeof(path))) {
        FILINFO info;
        if (!remove) result = fat_error(f_mkdir(path));
        else if ((result = fat_error(f_stat(path, &info))) == 0) {
            if (!!(info.fattrib & AM_DIR) != directory) { errno = directory ? ENOTDIR : EISDIR; result = -1; }
            else result = fat_error(f_unlink(path));
        }
    }
    unlock(); return result;
}
static int usb_unlink(void *ctx, const char *name) { (void)ctx; return usb_path_operation(name, false, true); }
static int usb_rmdir(void *ctx, const char *name) { (void)ctx; return usb_path_operation(name, true, true); }
static int usb_mkdir(void *ctx, const char *name, mode_t mode)
{ (void)ctx; (void)mode; return usb_path_operation(name, true, false); }

static int usb_rename(void *ctx, const char *source, const char *destination)
{
    (void)ctx;
    lock();
    char from[SOLAR_OS_STORAGE_PATH_MAX + 4], to[SOLAR_OS_STORAGE_PATH_MAX + 4];
    int result = fat_path(source, from, sizeof(from)) && fat_path(destination, to, sizeof(to)) ?
        fat_error(f_rename(from, to)) : -1;
    unlock(); return result;
}

static DIR *usb_opendir(void *ctx, const char *name)
{
    (void)ctx;
    lock();
    usb_dir_t *dir = NULL;
    char path[SOLAR_OS_STORAGE_PATH_MAX + 4];
    if (fat_path(name, path, sizeof(path))) {
        dir = solar_os_memory_calloc(1, sizeof(*dir), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "usb.storage.dir");
        if (!dir) errno = ENOMEM;
        else if (fat_error(f_opendir(&dir->directory, path))) { solar_os_memory_free(dir); dir = NULL; }
        else { dir->generation = generation; open_handles++; }
    }
    unlock(); return (DIR *)dir;
}

static struct dirent *usb_readdir(void *ctx, DIR *handle)
{
    (void)ctx;
    usb_dir_t *dir = (usb_dir_t *)handle;
    lock();
    struct dirent *result = NULL;
    if (!available() || dir->generation != generation) errno = ENODEV;
    else {
        FILINFO info;
        if (!fat_error(f_readdir(&dir->directory, &info))) {
            errno = 0;
            if (info.fname[0]) {
                memset(&dir->entry, 0, sizeof(dir->entry));
                strlcpy(dir->entry.d_name, info.fname, sizeof(dir->entry.d_name));
                dir->entry.d_type = (info.fattrib & AM_DIR) ? DT_DIR : DT_REG;
                dir->position++; result = &dir->entry;
            }
        }
    }
    unlock(); return result;
}

static int usb_readdir_r(void *ctx, DIR *handle, struct dirent *entry, struct dirent **out)
{
    lock();
    struct dirent *result = usb_readdir(ctx, handle);
    int error = result ? 0 : errno;
    *out = NULL;
    if (result) { *entry = *result; *out = entry; }
    unlock(); return error;
}

static int usb_closedir(void *ctx, DIR *handle)
{
    (void)ctx;
    usb_dir_t *dir = (usb_dir_t *)handle;
    lock();
    bool current = dir->generation == generation;
    int result = available() && current ? fat_error(f_closedir(&dir->directory)) : (errno = ENODEV, -1);
    if (current) open_handles--;
    solar_os_memory_free(dir);
    unlock(); return result;
}

static const esp_vfs_t usb_vfs = {
    .flags = ESP_VFS_FLAG_CONTEXT_PTR,
    .open_p = usb_open, .close_p = usb_close, .read_p = usb_read, .write_p = usb_write,
    .lseek_p = usb_lseek, .fsync_p = usb_fsync, .fstat_p = usb_fstat,
    .stat_p = usb_stat, .unlink_p = usb_unlink, .rmdir_p = usb_rmdir, .mkdir_p = usb_mkdir,
    .rename_p = usb_rename, .opendir_p = usb_opendir, .readdir_p = usb_readdir,
    .readdir_r_p = usb_readdir_r, .closedir_p = usb_closedir,
};

static DSTATUS usb_disk_status(BYTE drive)
{ return drive == volume && solar_os_usb_msc_media_current(&geometry) ?
    (geometry.write_protected ? STA_PROTECT : 0) : STA_NOINIT; }
static DRESULT usb_disk_read(BYTE drive, BYTE *data, LBA_t sector, UINT count)
{ return (usb_disk_status(drive) & STA_NOINIT) ? RES_NOTRDY : solar_os_usb_msc_read(sector, data,
    (size_t)count * geometry.sector_size) == ESP_OK ? RES_OK : RES_ERROR; }
static DRESULT usb_disk_write(BYTE drive, const BYTE *data, LBA_t sector, UINT count)
{ return (usb_disk_status(drive) & STA_NOINIT) ? RES_NOTRDY :
    (usb_disk_status(drive) & STA_PROTECT) ? RES_WRPRT : solar_os_usb_msc_write(sector, data,
    (size_t)count * geometry.sector_size) == ESP_OK ? RES_OK : RES_ERROR; }
static DRESULT usb_disk_ioctl(BYTE drive, BYTE command, void *data)
{
    if (usb_disk_status(drive) & STA_NOINIT) return RES_NOTRDY;
    /* BOT writes complete synchronously, matching Espressif's MSC diskio.
     * This does not promise persistence through loss of device power. */
    if (command == CTRL_SYNC) return RES_OK;
    if (!data) return RES_PARERR;
    switch (command) {
    case GET_SECTOR_COUNT: *(LBA_t *)data = geometry.sector_count; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)data = geometry.sector_size; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)data = 1; return RES_OK;
    default: return RES_PARERR;
    }
}
static const ff_diskio_impl_t diskio = {
    .init = usb_disk_status, .status = usb_disk_status, .read = usb_disk_read,
    .write = usb_disk_write, .ioctl = usb_disk_ioctl,
};

static void unmount_locked(void)
{
    if (!filesystem) return;
    char drive[5]; snprintf(drive, sizeof(drive), "%u:", volume);
    (void)f_mount(NULL, drive, 0);
    ff_diskio_unregister(volume);
    VolToPart[volume].pd = volume; VolToPart[volume].pt = 0;
    solar_os_memory_free(filesystem); filesystem = NULL; volume = UINT8_MAX;
    ++generation; open_handles = 0;
    solar_os_storage_notify_changed();
}

static esp_err_t refresh_locked(bool *changed)
{
    solar_os_usb_msc_info_t updated = geometry;
    esp_err_t err = solar_os_usb_msc_refresh(&updated);
    *changed = geometry.media_revision != updated.media_revision || geometry.media_present != updated.media_present;
    if (*changed || (geometry.ufi_cbi && err != ESP_OK)) unmount_locked();
    geometry = updated;
    if (*changed) ejected = false;
    last_media_probe = esp_timer_get_time();
    return err;
}

static esp_err_t mount_locked(void)
{
    if (!enabled || !address || !solar_os_usb_msc_present() || atomic_load(&removed)) return ESP_ERR_INVALID_STATE;
    bool changed;
    esp_err_t media_err = refresh_locked(&changed);
    if (media_err != ESP_OK) return media_err;
    if (filesystem) return available() ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (!geometry.media_present) return ESP_ERR_INVALID_STATE;
    if (!geometry.sector_count || geometry.sector_size < FF_MIN_SS || geometry.sector_size > FF_MAX_SS ||
        (geometry.sector_size & (geometry.sector_size - 1))) return ESP_ERR_NOT_SUPPORTED;
    FATFS *fs = solar_os_memory_calloc(1, sizeof(*fs), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "usb.storage.fs");
    if (!fs) return ESP_ERR_NO_MEM;
    if (ff_diskio_get_drive(&volume) != ESP_OK) { solar_os_memory_free(fs); return ESP_ERR_NO_MEM; }
    ff_diskio_register(volume, &diskio);
    VolToPart[volume].pd = volume; VolToPart[volume].pt = 0; /* Whole FAT volume or first FAT MBR partition. */
    char drive[5]; snprintf(drive, sizeof(drive), "%u:", volume);
    FRESULT result = f_mount(fs, drive, 1);
    if (result != FR_OK) {
        (void)f_mount(NULL, drive, 0); ff_diskio_unregister(volume);
        volume = UINT8_MAX; solar_os_memory_free(fs);
        return result == FR_NO_FILESYSTEM ? ESP_ERR_NOT_SUPPORTED : ESP_FAIL;
    }
    filesystem = fs; ++generation; ejected = false;
    solar_os_storage_notify_changed();
    return ESP_OK;
}

static void media_event(bool connected, uint8_t new_address)
{
    if (connected) atomic_store(&pending_address, new_address);
    else atomic_store(&removed, true);
}

static void media_poll(void)
{
    solar_os_disk_image_registry_lock(); lock();
    if (atomic_load(&removed)) {
        unmount_locked();
        last_error = solar_os_usb_msc_close();
        if (last_error == ESP_OK) { address = 0; atomic_store(&removed, false); }
    }
    unsigned next = enabled && !address && !atomic_load(&removed) ? atomic_exchange(&pending_address, 0) : 0;
    if (next) {
        last_error = solar_os_usb_msc_open(next, &geometry);
        if (last_error == ESP_OK) {
            address = next; ejected = false;
            last_error = geometry.media_present ? mount_locked() : ESP_OK;
            last_media_probe = esp_timer_get_time();
        }
        else {
            esp_err_t cleanup = solar_os_usb_msc_close();
            if (cleanup != ESP_OK) { address = next; last_error = cleanup; atomic_store(&removed, true); }
        }
    }
    if (enabled && address && geometry.ufi_cbi && !atomic_load(&removed) &&
        esp_timer_get_time() - last_media_probe >= 1000000) {
        bool changed;
        esp_err_t refresh_error = refresh_locked(&changed);
        if (refresh_error != ESP_OK || changed) last_error = refresh_error;
        if (refresh_error == ESP_OK && changed && !ejected && geometry.media_present) last_error = mount_locked();
    }
    unlock(); solar_os_disk_image_registry_unlock();
}

static void media_task(void *arg)
{
    (void)arg;
    while (!atomic_load(&stop_worker)) { media_poll(); vTaskDelay(pdMS_TO_TICKS(USB_POLL_MS)); }
    atomic_store(&worker_done, true);
    solar_os_task_delete(NULL);
}

esp_err_t solar_os_usb_storage_init(void)
{
    lock();
    esp_err_t err = ESP_OK;
    if (!vfs_registered) {
        err = esp_vfs_register(SOLAR_OS_USB_STORAGE_MOUNT_POINT, &usb_vfs, NULL);
        vfs_registered = err == ESP_OK;
    }
    unlock(); return err;
}

esp_err_t solar_os_usb_storage_start(void)
{
    lock();
    if (started) { unlock(); return ESP_ERR_INVALID_STATE; }
    esp_err_t err = solar_os_usb_storage_init();
    atomic_store(&pending_address, 0); atomic_store(&removed, false);
    atomic_store(&stop_worker, false);
    if (err == ESP_OK) err = solar_os_usb_msc_start(media_event);
    if (err == ESP_OK) {
        started = enabled = true;
        atomic_store(&worker_done, false);
        if (solar_os_task_create_pinned(media_task, "usb-storage", 4096, NULL, 2,
            NULL, 0, SOLAR_OS_TASK_ROLE_SYSTEM) != pdPASS) {
            atomic_store(&worker_done, true); enabled = false;
            esp_err_t cleanup = solar_os_usb_msc_stop();
            if (cleanup == ESP_OK) started = false;
            err = ESP_ERR_NO_MEM;
        }
    }
    last_error = err;
    unlock(); return err;
}

esp_err_t solar_os_usb_storage_stop(void)
{
    /* Registry -> guard is the same order used by disk mount/unmount. */
    solar_os_disk_image_registry_lock(); lock();
    esp_err_t err = ESP_OK;
    if (started) {
        if (available() && (open_handles || solar_os_disk_image_backing_busy(SOLAR_OS_USB_STORAGE_MOUNT_POINT)))
            err = ESP_ERR_INVALID_STATE;
        else {
            enabled = false;
            unmount_locked();
            err = solar_os_usb_msc_close();
            if (err == ESP_OK) atomic_store(&stop_worker, true);
        }
    }
    unlock(); solar_os_disk_image_registry_unlock();
    if (err != ESP_OK || !started) return err;
    int64_t deadline = esp_timer_get_time() + 10000000;
    while (!atomic_load(&worker_done)) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(USB_POLL_MS));
    }
    lock();
    err = solar_os_usb_msc_stop();
    if (err == ESP_OK) { started = false; address = 0; }
    last_error = err;
    unlock(); return err;
}

esp_err_t solar_os_usb_storage_mount(void)
{
    solar_os_disk_image_registry_lock(); lock();
    last_error = mount_locked(); esp_err_t err = last_error;
    unlock(); solar_os_disk_image_registry_unlock(); return err;
}

esp_err_t solar_os_usb_storage_unmount(void)
{
    solar_os_disk_image_registry_lock(); lock();
    esp_err_t err = !filesystem || open_handles ||
        solar_os_disk_image_backing_busy(SOLAR_OS_USB_STORAGE_MOUNT_POINT) ? ESP_ERR_INVALID_STATE : ESP_OK;
    if (err == ESP_OK) { unmount_locked(); ejected = true; }
    last_error = err;
    unlock(); solar_os_disk_image_registry_unlock(); return err;
}

void solar_os_usb_storage_get_status(solar_os_usb_storage_status_t *status)
{
    if (!status) return;
    lock();
    *status = (solar_os_usb_storage_status_t){.connected = address && solar_os_usb_msc_present(),
        .mounted = available(), .address = address,
        .ufi_cbi = geometry.ufi_cbi, .media_present = geometry.media_present,
        .write_protected = geometry.write_protected,
        .vendor_id = geometry.vendor_id, .product_id = geometry.product_id,
        .sector_size = geometry.sector_size, .sector_count = geometry.sector_count,
        .open_handles = open_handles, .last_error = last_error};
    unlock();
}

bool solar_os_usb_storage_get_block(solar_os_storage_block_t *block)
{
    if (!block) return false;
    lock();
    bool connected = address && solar_os_usb_msc_present();
    if (connected) {
        memset(block, 0, sizeof(*block));
        strlcpy(block->name, "usb0", sizeof(block->name));
        if (geometry.media_present) strlcpy(block->fs, "FAT", sizeof(block->fs));
        strlcpy(block->type_name, "usb", sizeof(block->type_name));
        block->type = SOLAR_OS_STORAGE_BLOCK_DISK; block->mountable = true;
        block->mounted = available();
        /* Keep raw FatFS access private: all consumers must use the guarded VFS. */
        block->logical_volume = SOLAR_OS_STORAGE_LOGICAL_VOLUME_INVALID;
        block->sector_size = geometry.sector_size; block->sector_count = geometry.sector_count;
        block->size_bytes = (uint64_t)geometry.sector_size * geometry.sector_count;
        if (block->mounted) strlcpy(block->mount_point, SOLAR_OS_USB_STORAGE_MOUNT_POINT, sizeof(block->mount_point));
    }
    unlock(); return connected;
}

esp_err_t solar_os_usb_storage_get_usage(solar_os_storage_usage_t *usage)
{
    if (!usage) return ESP_ERR_INVALID_ARG;
    lock();
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (available()) {
        char drive[5]; snprintf(drive, sizeof(drive), "%u:", volume);
        DWORD clusters; FATFS *fs;
        if (f_getfree(drive, &clusters, &fs) == FR_OK) {
            usage->total_bytes = (uint64_t)(fs->n_fatent - 2) * fs->csize * geometry.sector_size;
            usage->free_bytes = (uint64_t)clusters * fs->csize * geometry.sector_size;
            usage->used_bytes = usage->total_bytes - usage->free_bytes;
            err = ESP_OK;
        } else err = ESP_FAIL;
    }
    unlock(); return err;
}
