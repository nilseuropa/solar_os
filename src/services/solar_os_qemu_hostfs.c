#include "solar_os_qemu_hostfs.h"
#include "solar_os_qemu_hostfs_protocol.h"
#include "solar_os_memory.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <utime.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "soc/soc.h"

static struct {
    bool mounted;
    SemaphoreHandle_t lock;
    StaticSemaphore_t lock_storage;
    uint32_t files, dirs;
} hostfs;

typedef struct {
    DIR dir;
    unsigned handle;
    struct dirent entry;
} hostfs_dir_t;

static uint32_t read_reg(uint32_t offset)
{ return REG_READ(SOLAR_OS_QEMU_HOSTFS_BASE + offset); }
static void write_reg(uint32_t offset, uint32_t value)
{ REG_WRITE(SOLAR_OS_QEMU_HOSTFS_BASE + offset, value); }

static void transfer(void *buffer, size_t length, bool write)
{
    uint8_t *bytes = buffer;
    for (size_t i = 0; i < length; i += 4) {
        uint32_t word = 0;
        size_t count = length - i < 4 ? length - i : 4;
        if (write) {
            memcpy(&word, bytes + i, count);
            write_reg(SOLAR_OS_QEMU_HOSTFS_REG_DATA + i, word);
        } else {
            word = read_reg(SOLAR_OS_QEMU_HOSTFS_REG_DATA + i);
            memcpy(bytes + i, &word, count);
        }
    }
}

static int decode_error(unsigned error)
{
    switch (error) {
#define HOSTFS_ERROR_CASE(name) case SOLAR_OS_QEMU_HOSTFS_ERROR_##name: return name;
    SOLAR_OS_QEMU_HOSTFS_ERRORS(HOSTFS_ERROR_CASE)
#undef HOSTFS_ERROR_CASE
    default: return EIO;
    }
}

static int rpc(unsigned command, uint32_t args[4], const void *input, size_t in_size,
               void *output, size_t out_size)
{
    xSemaphoreTake(hostfs.lock, portMAX_DELAY);
    if (!hostfs.mounted && command != SOLAR_OS_QEMU_HOSTFS_RESET) {
        xSemaphoreGive(hostfs.lock); errno = EIO; return -1;
    }
    if (input) transfer((void *)input, in_size, true);
    for (unsigned i = 0; i < 4; i++) write_reg(SOLAR_OS_QEMU_HOSTFS_REG_ARG0 + i * 4, args[i]);
    const unsigned handle = args[0];
    write_reg(SOLAR_OS_QEMU_HOSTFS_REG_COMMAND, command);
    const int result = (int32_t)read_reg(SOLAR_OS_QEMU_HOSTFS_REG_RESULT);
    if (result >= 0) {
        if (output) transfer(output, command == SOLAR_OS_QEMU_HOSTFS_READ ? (size_t)result : out_size, false);
        for (unsigned i = 0; i < 4; i++) args[i] = read_reg(SOLAR_OS_QEMU_HOSTFS_REG_ARG0 + i * 4);
        if (command == SOLAR_OS_QEMU_HOSTFS_OPEN && result > 0 && result <= 32) hostfs.files |= 1U << (result - 1);
        if (command == SOLAR_OS_QEMU_HOSTFS_OPENDIR && result > 0 && result <= 8) hostfs.dirs |= 1U << (result - 1);
    } else errno = decode_error(read_reg(SOLAR_OS_QEMU_HOSTFS_REG_ERROR));
    if (command == SOLAR_OS_QEMU_HOSTFS_CLOSE && handle && handle <= 32) hostfs.files &= ~(1U << (handle - 1));
    if (command == SOLAR_OS_QEMU_HOSTFS_CLOSEDIR && handle && handle <= 8) hostfs.dirs &= ~(1U << (handle - 1));
    if (command == SOLAR_OS_QEMU_HOSTFS_RESET) hostfs.files = hostfs.dirs = 0;
    xSemaphoreGive(hostfs.lock);
    return result;
}

static int path_rpc(unsigned command, const char *path, const char *other, uint32_t args[4],
                    void *output, size_t size)
{
    char paths[SOLAR_OS_QEMU_HOSTFS_PATH_SIZE * 2];
    if (!path) { errno = EINVAL; return -1; }
    size_t first = strnlen(path, SOLAR_OS_QEMU_HOSTFS_PATH_SIZE);
    size_t second = other ? strnlen(other, SOLAR_OS_QEMU_HOSTFS_PATH_SIZE) : 0;
    if (first == SOLAR_OS_QEMU_HOSTFS_PATH_SIZE || second == SOLAR_OS_QEMU_HOSTFS_PATH_SIZE) {
        errno = ENAMETOOLONG; return -1;
    }
    memcpy(paths, path, first + 1);
    if (other) memcpy(paths + first + 1, other, second + 1);
    return rpc(command, args, paths, first + 1 + (other ? second + 1 : 0), output, size);
}

static int host_open(const char *path, int flags, int mode)
{
    unsigned wire = (flags & O_ACCMODE) == O_RDWR ? SOLAR_OS_QEMU_HOSTFS_O_RDWR :
        (flags & O_ACCMODE) == O_WRONLY ? SOLAR_OS_QEMU_HOSTFS_O_WRITE : 0;
    if (flags & O_CREAT) wire |= SOLAR_OS_QEMU_HOSTFS_O_CREATE;
    if (flags & O_TRUNC) wire |= SOLAR_OS_QEMU_HOSTFS_O_TRUNCATE;
    if (flags & O_APPEND) wire |= SOLAR_OS_QEMU_HOSTFS_O_APPEND;
    if (flags & O_EXCL) wire |= SOLAR_OS_QEMU_HOSTFS_O_EXCLUSIVE;
    uint32_t args[4] = {wire, mode};
    return path_rpc(SOLAR_OS_QEMU_HOSTFS_OPEN, path, NULL, args, NULL, 0);
}

static int fd_rpc(unsigned command, int fd)
{
    uint32_t args[4] = {fd};
    return rpc(command, args, NULL, 0, NULL, 0);
}
static int host_close(int fd) { return fd_rpc(SOLAR_OS_QEMU_HOSTFS_CLOSE, fd); }
static int host_fsync(int fd) { return fd_rpc(SOLAR_OS_QEMU_HOSTFS_FSYNC, fd); }

static ssize_t file_io(int fd, void *buffer, size_t size, bool write)
{
    size_t done = 0;
    if (size > (size_t)INT_MAX) { errno = EINVAL; return -1; }
    while (done < size) {
        size_t chunk = size - done < SOLAR_OS_QEMU_HOSTFS_DATA_SIZE ? size - done : SOLAR_OS_QEMU_HOSTFS_DATA_SIZE;
        uint32_t args[4] = {fd, chunk};
        void *data = (uint8_t *)buffer + done;
        int count = rpc(write ? SOLAR_OS_QEMU_HOSTFS_WRITE : SOLAR_OS_QEMU_HOSTFS_READ,
            args, write ? data : NULL, write ? chunk : 0, write ? NULL : data, write ? 0 : chunk);
        if (count < 0) return done ? (ssize_t)done : -1;
        if (!count) break;
        done += count;
        if ((size_t)count < chunk) break;
    }
    return done;
}
static ssize_t host_read(int fd, void *data, size_t size) { return file_io(fd, data, size, false); }
static ssize_t host_write(int fd, const void *data, size_t size) { return file_io(fd, (void *)data, size, true); }

static off_t host_lseek(int fd, off_t offset, int whence)
{
    unsigned wire;
    switch (whence) {
    case SEEK_SET: wire = 0; break;
    case SEEK_CUR: wire = 1; break;
    case SEEK_END: wire = 2; break;
    default: errno = EINVAL; return -1;
    }
    uint32_t args[4] = {fd, (uint64_t)offset, (uint64_t)offset >> 32, wire};
    if (rpc(SOLAR_OS_QEMU_HOSTFS_SEEK, args, NULL, 0, NULL, 0) < 0) return -1;
    uint64_t result = ((uint64_t)args[1] << 32) | args[0];
    off_t position = result;
    if (position < 0 || (uint64_t)position != result) { errno = EOVERFLOW; return -1; }
    return position;
}

static uint64_t unpack64(const uint32_t *words) { return ((uint64_t)words[1] << 32) | words[0]; }
static int decode_stat(const uint32_t data[10], struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = data[1] | (data[0] == SOLAR_OS_QEMU_HOSTFS_TYPE_DIR ? S_IFDIR :
        data[0] == SOLAR_OS_QEMU_HOSTFS_TYPE_FILE ? S_IFREG : 0);
    st->st_size = unpack64(data + 2);
    if (st->st_size < 0 || (uint64_t)st->st_size != unpack64(data + 2)) { errno = EOVERFLOW; return -1; }
    st->st_mtime = (int64_t)unpack64(data + 4);
    st->st_atime = (int64_t)unpack64(data + 6);
    st->st_ctime = (int64_t)unpack64(data + 8);
    st->st_blksize = 4096;
    st->st_nlink = 1;
    return 0;
}
static int host_stat(const char *path, struct stat *st)
{
    uint32_t data[10], args[4] = {0};
    return path_rpc(SOLAR_OS_QEMU_HOSTFS_STAT, path, NULL, args, data, sizeof(data)) < 0 ? -1 : decode_stat(data, st);
}
static int host_fstat(int fd, struct stat *st)
{
    uint32_t data[10], args[4] = {fd};
    return rpc(SOLAR_OS_QEMU_HOSTFS_FSTAT, args, NULL, 0, data, sizeof(data)) < 0 ? -1 : decode_stat(data, st);
}

static int host_unlink(const char *path)
{ return path_rpc(SOLAR_OS_QEMU_HOSTFS_UNLINK, path, NULL, (uint32_t[4]){0}, NULL, 0); }
static int host_rename(const char *src, const char *dst)
{ return path_rpc(SOLAR_OS_QEMU_HOSTFS_RENAME, src, dst, (uint32_t[4]){0}, NULL, 0); }
static int host_mkdir(const char *path, mode_t mode)
{ return path_rpc(SOLAR_OS_QEMU_HOSTFS_MKDIR, path, NULL, (uint32_t[4]){mode}, NULL, 0); }
static int host_rmdir(const char *path)
{ return path_rpc(SOLAR_OS_QEMU_HOSTFS_RMDIR, path, NULL, (uint32_t[4]){0}, NULL, 0); }
static int host_access(const char *path, int mode)
{
    struct stat st;
    if (mode & ~(R_OK | W_OK | X_OK)) { errno = EINVAL; return -1; }
    if (host_stat(path, &st)) return -1;
    if (((mode & R_OK) && !(st.st_mode & 0444)) || ((mode & W_OK) && !(st.st_mode & 0222)) ||
        ((mode & X_OK) && !(st.st_mode & 0111))) { errno = EACCES; return -1; }
    return 0;
}
static int host_ftruncate(int fd, off_t size)
{
    uint32_t args[4] = {fd, (uint64_t)size, (uint64_t)size >> 32};
    return rpc(SOLAR_OS_QEMU_HOSTFS_TRUNCATE, args, NULL, 0, NULL, 0);
}
static int host_truncate(const char *path, off_t size)
{
    int fd = host_open(path, O_WRONLY, 0);
    if (fd < 0) return -1;
    int result = host_ftruncate(fd, size), saved = errno;
    host_close(fd);
    errno = saved;
    return result;
}
static int host_utime(const char *path, const struct utimbuf *times)
{
    time_t atime = times ? times->actime : time(NULL), mtime = times ? times->modtime : atime;
    uint32_t args[4] = {(uint64_t)atime, (uint64_t)atime >> 32, (uint64_t)mtime, (uint64_t)mtime >> 32};
    return path_rpc(SOLAR_OS_QEMU_HOSTFS_UTIME, path, NULL, args, NULL, 0);
}

static DIR *host_opendir(const char *path)
{
    hostfs_dir_t *dir = solar_os_memory_calloc(1, sizeof(*dir),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "qemu-hostfs.dir");
    if (!dir) { errno = ENOMEM; return NULL; }
    int id = path_rpc(SOLAR_OS_QEMU_HOSTFS_OPENDIR, path, NULL, (uint32_t[4]){0}, NULL, 0);
    if (id < 0) { solar_os_memory_free(dir); return NULL; }
    dir->handle = id;
    return &dir->dir;
}
static struct dirent *host_readdir(DIR *stream)
{
    hostfs_dir_t *dir = (hostfs_dir_t *)stream;
    uint32_t args[4] = {dir->handle};
    uint8_t data[4 + sizeof(dir->entry.d_name)];
    int result = rpc(SOLAR_OS_QEMU_HOSTFS_READDIR, args, NULL, 0, data, sizeof(data));
    if (result <= 0) { if (!result) errno = 0; return NULL; }
    uint32_t type;
    memcpy(&type, data, 4);
    memset(&dir->entry, 0, sizeof(dir->entry));
    dir->entry.d_type = type == SOLAR_OS_QEMU_HOSTFS_TYPE_DIR ? DT_DIR :
        type == SOLAR_OS_QEMU_HOSTFS_TYPE_FILE ? DT_REG : DT_UNKNOWN;
    memcpy(dir->entry.d_name, data + 4, sizeof(dir->entry.d_name) - 1);
    return &dir->entry;
}
static int host_readdir_r(DIR *dir, struct dirent *entry, struct dirent **out)
{
    errno = 0;
    struct dirent *next = host_readdir(dir);
    if (next) *entry = *next;
    *out = next ? entry : NULL;
    return next ? 0 : errno;
}
static long host_telldir(DIR *dir)
{ return fd_rpc(SOLAR_OS_QEMU_HOSTFS_TELLDIR, ((hostfs_dir_t *)dir)->handle); }
static void host_seekdir(DIR *dir, long offset)
{
    if (offset < 0) { errno = EINVAL; return; }
    uint32_t args[4] = {((hostfs_dir_t *)dir)->handle, offset};
    rpc(SOLAR_OS_QEMU_HOSTFS_SEEKDIR, args, NULL, 0, NULL, 0);
}
static int host_closedir(DIR *stream)
{
    hostfs_dir_t *dir = (hostfs_dir_t *)stream;
    int result = fd_rpc(SOLAR_OS_QEMU_HOSTFS_CLOSEDIR, dir->handle);
    solar_os_memory_free(dir);
    return result;
}

static const esp_vfs_t host_vfs = {
    .flags = ESP_VFS_FLAG_DEFAULT,
    .open = host_open, .close = host_close, .read = host_read, .write = host_write,
    .lseek = host_lseek, .fstat = host_fstat, .fsync = host_fsync,
    .stat = host_stat, .unlink = host_unlink, .rename = host_rename,
    .mkdir = host_mkdir, .rmdir = host_rmdir, .access = host_access,
    .truncate = host_truncate, .ftruncate = host_ftruncate, .utime = host_utime,
    .opendir = host_opendir, .readdir = host_readdir, .readdir_r = host_readdir_r,
    .telldir = host_telldir, .seekdir = host_seekdir, .closedir = host_closedir,
};

bool solar_os_qemu_hostfs_is_mounted(void) { return hostfs.mounted; }
esp_err_t solar_os_qemu_hostfs_mount(void)
{
    if (hostfs.mounted) return ESP_OK;
    if (read_reg(SOLAR_OS_QEMU_HOSTFS_REG_MAGIC) != SOLAR_OS_QEMU_HOSTFS_MAGIC ||
        read_reg(SOLAR_OS_QEMU_HOSTFS_REG_VERSION) != SOLAR_OS_QEMU_HOSTFS_VERSION ||
        !read_reg(SOLAR_OS_QEMU_HOSTFS_REG_READY)) return ESP_ERR_NOT_SUPPORTED;
    if (!hostfs.lock) hostfs.lock = xSemaphoreCreateMutexStatic(&hostfs.lock_storage);
    if (!hostfs.lock) return ESP_ERR_NO_MEM;
    if (rpc(SOLAR_OS_QEMU_HOSTFS_RESET, (uint32_t[4]){0}, NULL, 0, NULL, 0)) return ESP_FAIL;
    hostfs.mounted = true;
    esp_err_t err = esp_vfs_register(SOLAR_OS_QEMU_HOSTFS_MOUNT, &host_vfs, NULL);
    if (err != ESP_OK) hostfs.mounted = false;
    else ESP_LOGI("qemu_hostfs", "host directory mounted at " SOLAR_OS_QEMU_HOSTFS_MOUNT);
    return err;
}

esp_err_t solar_os_qemu_hostfs_unmount(void)
{
    if (!hostfs.mounted) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(hostfs.lock, portMAX_DELAY);
    esp_err_t err = hostfs.files || hostfs.dirs ? ESP_ERR_INVALID_STATE :
        esp_vfs_unregister(SOLAR_OS_QEMU_HOSTFS_MOUNT);
    if (err == ESP_OK) hostfs.mounted = false;
    xSemaphoreGive(hostfs.lock);
    return err;
}

esp_err_t solar_os_qemu_hostfs_get_usage(solar_os_storage_usage_t *usage)
{
    if (!usage) return ESP_ERR_INVALID_ARG;
    if (!hostfs.mounted) return ESP_ERR_INVALID_STATE;
    uint32_t data[6], args[4] = {0};
    if (rpc(SOLAR_OS_QEMU_HOSTFS_USAGE, args, NULL, 0, data, sizeof(data))) return ESP_FAIL;
    usage->total_bytes = unpack64(data);
    usage->used_bytes = unpack64(data + 2);
    usage->free_bytes = unpack64(data + 4);
    return ESP_OK;
}
