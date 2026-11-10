#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include "../../src/services/solar_os_usb_storage.c"

static const ff_diskio_impl_t *impls[FF_VOLUMES];
PARTITION VolToPart[FF_VOLUMES];
static pthread_mutex_t lock_init = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t fat_locks[FF_VOLUMES + 1];
static FILE *media;
static solar_os_usb_msc_callback_t callback;
static bool port_started, port_open, port_present, backing_busy, fail_task, fail_io;
static unsigned writes, revisions, reservations;
static int64_t test_clock;
static solar_os_usb_msc_info_t test_geometry = {.sector_size = 512, .sector_count = 131072,
    .vendor_id = 0x1234, .product_id = 0x5678, .media_present = true};

void _lock_acquire_recursive(_lock_t *mutex)
{
    pthread_mutex_lock(&lock_init);
    if (!*mutex) {
        *mutex = malloc(sizeof(**mutex)); assert(*mutex);
        pthread_mutexattr_t attr; pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(*mutex, &attr); pthread_mutexattr_destroy(&attr);
    }
    pthread_mutex_unlock(&lock_init);
    pthread_mutex_lock(*mutex);
}
void _lock_release_recursive(_lock_t *mutex) { pthread_mutex_unlock(*mutex); }
int ff_mutex_create(int vol) { return !pthread_mutex_init(&fat_locks[vol], NULL); }
void ff_mutex_delete(int vol) { pthread_mutex_destroy(&fat_locks[vol]); }
int ff_mutex_take(int vol) { return !pthread_mutex_lock(&fat_locks[vol]); }
void ff_mutex_give(int vol) { pthread_mutex_unlock(&fat_locks[vol]); }
void *ff_memalloc(unsigned size) { return malloc(size); }
void ff_memfree(void *p) { free(p); }
DWORD get_fattime(void) { return (46U << 25) | (11U << 21) | (10U << 16); }
void vTaskDelay(unsigned ticks) { (void)ticks; }
int64_t esp_timer_get_time(void) { return test_clock; }
int solar_os_task_create_pinned(void (*task)(void *), const char *name, uint32_t stack,
    void *arg, unsigned priority, void *handle, int core, int role)
{
    (void)task; (void)name; (void)stack; (void)arg; (void)priority; (void)handle; (void)core; (void)role;
    /* Tests drive media_poll deterministically instead of starting a worker. */
    atomic_store(&worker_done, true);
    return fail_task ? 0 : pdPASS;
}
void solar_os_task_delete(void *handle) { (void)handle; }
void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t cls, const char *tag)
{ (void)cls; (void)tag; return calloc(count, size); }
void solar_os_memory_free(void *p) { free(p); }
size_t strlcpy(char *out, const char *in, size_t size)
{ size_t n = strlen(in); if (size) { size_t c = n < size ? n : size - 1; memcpy(out, in, c); out[c] = 0; } return n; }
esp_err_t esp_vfs_register(const char *path, const esp_vfs_t *vfs, void *ctx)
{ assert(!strcmp(path, "/usb0") && vfs == &usb_vfs && !ctx); reservations++; return ESP_OK; }
void solar_os_storage_notify_changed(void) { revisions++; }
void solar_os_disk_image_registry_lock(void) {}
void solar_os_disk_image_registry_unlock(void) {}
bool solar_os_disk_image_backing_busy(const char *path)
{ assert(!strcmp(path, "/usb0")); return backing_busy; }

esp_err_t solar_os_usb_msc_start(solar_os_usb_msc_callback_t cb)
{ assert(!port_started); port_started = true; callback = cb; return ESP_OK; }
esp_err_t solar_os_usb_msc_stop(void)
{ assert(port_started && !port_open && !filesystem); port_started = false; return ESP_OK; }
esp_err_t solar_os_usb_msc_open(uint8_t addr, solar_os_usb_msc_info_t *info)
{ assert(port_started && !port_open && addr); port_open = port_present = true; *info = test_geometry; return ESP_OK; }
esp_err_t solar_os_usb_msc_close(void) { assert(!filesystem); port_open = port_present = false; return ESP_OK; }
bool solar_os_usb_msc_present(void) { return port_present; }
bool solar_os_usb_msc_media_current(const solar_os_usb_msc_info_t *info)
{ return port_present && (!info->ufi_cbi || (test_geometry.media_present && info->media_revision == test_geometry.media_revision)); }
esp_err_t solar_os_usb_msc_refresh(solar_os_usb_msc_info_t *info)
{ *info = test_geometry; return ESP_OK; }
esp_err_t solar_os_usb_msc_read(uint32_t sector, void *data, size_t bytes)
{
    if (!port_present || fail_io) return ESP_FAIL;
    assert(sector < test_geometry.sector_count && bytes <= (test_geometry.sector_count - sector) * 512ULL);
    return pread(fileno(media), data, bytes, (off_t)sector * 512) == (ssize_t)bytes ? ESP_OK : ESP_FAIL;
}
esp_err_t solar_os_usb_msc_write(uint32_t sector, const void *data, size_t bytes)
{
    if (!port_present || fail_io) return ESP_FAIL;
    writes++;
    assert(sector < test_geometry.sector_count && bytes <= (test_geometry.sector_count - sector) * 512ULL);
    return pwrite(fileno(media), data, bytes, (off_t)sector * 512) == (ssize_t)bytes ? ESP_OK : ESP_FAIL;
}
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
DRESULT disk_ioctl(BYTE drive, BYTE command, void *data) { return impls[drive]->ioctl(drive, command, data); }

static void format_media(unsigned sectors, BYTE format)
{
    assert(!filesystem && !port_open);
    if (media) fclose(media);
    media = tmpfile(); assert(media);
    test_geometry.sector_count = sectors;
    assert(!ftruncate(fileno(media), (off_t)sectors * 512));
    /* Fixture creation only: production has no format path. */
    geometry = test_geometry; volume = 3; port_present = true;
    ff_diskio_register(3, &diskio); VolToPart[3].pd = 3; VolToPart[3].pt = 0;
    BYTE work[4096]; MKFS_PARM options = {.fmt = format, .n_fat = 1};
    assert(f_mkfs("3:", &options, work, sizeof(work)) == FR_OK);
    ff_diskio_unregister(3); volume = UINT8_MAX; port_present = false;
}
static void connect_media(void) { callback(true, 1); media_poll(); assert(available()); }
static void disconnect_media(void)
{ port_present = false; callback(false, 0); media_poll(); assert(!filesystem && !address && !port_open); }

static void file_roundtrip(void)
{
    assert(!usb_mkdir(NULL, "/notes", 0777));
    int fd = usb_open(NULL, "/notes/test.txt", O_CREAT | O_TRUNC | O_RDWR, 0666); assert(fd >= 0);
    assert(usb_write(NULL, fd, "hello", 5) == 5);
    assert(!usb_fsync(NULL, fd));
    assert(solar_os_usb_storage_unmount() == ESP_ERR_INVALID_STATE);
    assert(solar_os_usb_storage_stop() == ESP_ERR_INVALID_STATE);
    assert(port_open && filesystem && enabled);
    assert(!usb_lseek(NULL, fd, 0, SEEK_SET));
    char data[16] = {0}; assert(usb_read(NULL, fd, data, sizeof(data)) == 5 && !strcmp(data, "hello"));
    struct stat st; assert(!usb_fstat(NULL, fd, &st) && st.st_size == 5 && st.st_ino && st.st_mtime > 0);
    assert(!usb_close(NULL, fd));
    fd = usb_open(NULL, "/notes/test.txt", O_WRONLY | O_APPEND, 0); assert(fd >= 0);
    assert(usb_write(NULL, fd, "!", 1) == 1 && !usb_close(NULL, fd));
    assert(!usb_stat(NULL, "/notes/test.txt", &st) && st.st_size == 6);
    assert(!usb_rename(NULL, "/notes/test.txt", "/notes/renamed.txt"));
    DIR *dir = usb_opendir(NULL, "/notes"); assert(dir);
    assert(solar_os_usb_storage_unmount() == ESP_ERR_INVALID_STATE);
    struct dirent *entry = usb_readdir(NULL, dir); assert(entry && !strcasecmp(entry->d_name, "renamed.txt"));
    errno = 999; assert(!usb_readdir(NULL, dir) && !errno); assert(!usb_closedir(NULL, dir));
    assert(solar_os_usb_storage_unmount() == ESP_OK && !filesystem);
    assert(usb_open(NULL, "/notes/test.txt", O_RDONLY, 0) == -1 && errno == ENODEV);
    assert(solar_os_usb_storage_mount() == ESP_OK);
    fd = usb_open(NULL, "/notes/renamed.txt", O_RDONLY, 0); assert(fd >= 0);
    memset(data, 0, sizeof(data)); assert(usb_read(NULL, fd, data, sizeof(data)) == 6 && !strcmp(data, "hello!"));
    assert(!usb_close(NULL, fd));
    solar_os_storage_usage_t usage; assert(solar_os_usb_storage_get_usage(&usage) == ESP_OK);
    assert(usage.total_bytes && usage.used_bytes && usage.free_bytes < usage.total_bytes);
}

static void floppy_media_changes(void)
{
    format_media(2880, FM_FAT | FM_SFD);
    test_geometry.ufi_cbi = true; test_geometry.media_present = false; test_geometry.sector_count = 0;
    callback(true, 1); media_poll();
    solar_os_usb_storage_status_t status;
    solar_os_usb_storage_get_status(&status);
    assert(status.connected && !status.mounted && !status.media_present && last_error == ESP_OK);
    test_geometry.media_present = true; test_geometry.sector_count = 2880; test_geometry.media_revision++;
    test_clock += 1000001; media_poll(); assert(available() && filesystem->fs_type == FS_FAT12);
    int old = usb_open(NULL, "/test.txt", O_CREAT | O_RDWR, 0666); assert(old >= 0);
    assert(usb_write(NULL, old, "disk", 4) == 4 && !usb_fsync(NULL, old));
    DIR *old_dir = usb_opendir(NULL, "/"); assert(old_dir);
    unsigned before = writes;
    test_geometry.media_revision++; /* Same-size replacement, USB still attached. */
    assert(usb_write(NULL, old, "wrong disk", 10) == -1 && errno == ENODEV);
    test_clock += 1000001; media_poll(); assert(available() && !open_handles && writes == before);
    assert(usb_close(NULL, old) == -1 && errno == ENODEV);
    assert(usb_closedir(NULL, old_dir) == -1 && errno == ENODEV);
    assert(solar_os_usb_storage_unmount() == ESP_OK);
    test_clock += 1000001; media_poll(); assert(!filesystem); /* Explicit eject stays ejected. */
    test_geometry.media_present = false; test_geometry.sector_count = 0; test_geometry.media_revision++;
    test_clock += 1000001; media_poll(); assert(!filesystem && address);
    test_geometry.media_present = true; test_geometry.sector_count = 2880;
    test_geometry.write_protected = true; test_geometry.media_revision++;
    test_clock += 1000001; media_poll(); assert(available());
    assert(usb_open(NULL, "/new.txt", O_CREAT | O_WRONLY, 0666) == -1 && errno == EROFS);
    int fd = usb_open(NULL, "/test.txt", O_RDONLY, 0); assert(fd >= 0);
    char data[5] = {0}; assert(usb_read(NULL, fd, data, 4) == 4 && !strcmp(data, "disk"));
    assert(!usb_close(NULL, fd) && writes == before);
    disconnect_media();
    test_geometry.ufi_cbi = false; test_geometry.write_protected = false;
}

int main(void)
{
    assert(solar_os_usb_storage_init() == ESP_OK && reservations == 1);
    assert(usb_open(NULL, "/file", O_CREAT | O_WRONLY, 0666) == -1 && errno == ENODEV);
    fail_task = true;
    assert(solar_os_usb_storage_start() == ESP_ERR_NO_MEM && !started && !port_started);
    fail_task = false;
    assert(solar_os_usb_storage_start() == ESP_OK && reservations == 1);
    /* Simulate flash already occupying logical drive zero. */
    impls[0] = &diskio;
    const unsigned sectors[] = {2048, 32768, 131072};
    for (unsigned i = 0; i < 3; ++i) {
        format_media(sectors[i], i == 2 ? FM_FAT32 : FM_FAT | (i ? 0 : FM_SFD));
        connect_media(); assert(volume == 1 && filesystem->fs_type == i + 1);
        solar_os_storage_block_t block; assert(solar_os_usb_storage_get_block(&block));
        assert(block.mounted && !strcmp(block.name, "usb0") && !strcmp(block.mount_point, "/usb0"));
        file_roundtrip(); disconnect_media();
    }
    floppy_media_changes();
    /* Stale files and directories survive detach only as closable handles. */
    format_media(131072, FM_FAT32); connect_media();
    int old = usb_open(NULL, "/old.txt", O_CREAT | O_RDWR, 0666); assert(old >= 0);
    DIR *old_dir = usb_opendir(NULL, "/"); assert(old_dir);
    assert(usb_write(NULL, old, "old", 3) == 3);
    disconnect_media();
    format_media(131072, FM_FAT32); connect_media();
    unsigned before = writes;
    assert(usb_write(NULL, old, "corrupt", 7) == -1 && errno == ENODEV);
    assert(!usb_readdir(NULL, old_dir) && errno == ENODEV);
    assert(usb_close(NULL, old) == -1 && errno == ENODEV);
    assert(usb_closedir(NULL, old_dir) == -1 && errno == ENODEV);
    assert(writes == before && open_handles == 0);
    backing_busy = true; assert(solar_os_usb_storage_stop() == ESP_ERR_INVALID_STATE);
    backing_busy = false;
    fail_io = true;
    assert(usb_mkdir(NULL, "/failure", 0777) == -1);
    fail_io = false;
    disconnect_media();
    /* Unsupported media is left byte-for-byte untouched, and can be unplugged. */
    fclose(media); media = tmpfile(); assert(media);
    assert(!ftruncate(fileno(media), 131072 * 512));
    BYTE marker[512]; memset(marker, 0x5a, sizeof(marker));
    assert(pwrite(fileno(media), marker, sizeof(marker), 0) == sizeof(marker));
    before = writes;
    callback(true, 1); media_poll();
    assert(address && !filesystem && last_error == ESP_ERR_NOT_SUPPORTED && writes == before);
    BYTE check[512]; assert(pread(fileno(media), check, sizeof(check), 0) == sizeof(check));
    assert(!memcmp(check, marker, sizeof(check)));
    disconnect_media();
    assert(solar_os_usb_storage_stop() == ESP_OK && !port_started);
    assert(usb_open(NULL, "/file", O_RDONLY, 0) == -1 && errno == ENODEV);
    assert(revisions > 10 && reservations == 1);
    fclose(media);
    pthread_mutex_destroy(guard); free(guard); guard = NULL;
    puts("USB FAT12/16/32, I/O, busy, hot-removal and stale-handle tests: ok");
    return 0;
}
