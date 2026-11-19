/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include <dirent.h>
#include <sys/statvfs.h>
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/module.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "solar_os_qemu_hostfs_protocol.h"

#define TYPE_SOLAR_OS_HOSTFS "solar-os-hostfs"
OBJECT_DECLARE_SIMPLE_TYPE(SolarOSHostFS, SOLAR_OS_HOSTFS)

struct SolarOSHostFS {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    char *root;
    int root_fd, files[SOLAR_OS_QEMU_HOSTFS_MAX_FILES];
    DIR *dirs[SOLAR_OS_QEMU_HOSTFS_MAX_DIRS];
    uint32_t positions[SOLAR_OS_QEMU_HOSTFS_MAX_DIRS];
    uint32_t args[4], result, error;
    uint8_t data[SOLAR_OS_QEMU_HOSTFS_DATA_SIZE];
};

static unsigned wire_error(int error)
{
    switch (error) {
#define HOSTFS_ERROR_CASE(name) case name: return SOLAR_OS_QEMU_HOSTFS_ERROR_##name;
    SOLAR_OS_QEMU_HOSTFS_ERRORS(HOSTFS_ERROR_CASE)
#undef HOSTFS_ERROR_CASE
    default: return SOLAR_OS_QEMU_HOSTFS_ERROR_EIO;
    }
}

static void close_handles(SolarOSHostFS *s)
{
    for (unsigned i = 0; i < G_N_ELEMENTS(s->files); i++) {
        if (s->files[i] >= 0) close(s->files[i]);
        s->files[i] = -1;
    }
    for (unsigned i = 0; i < G_N_ELEMENTS(s->dirs); i++) {
        if (s->dirs[i]) closedir(s->dirs[i]);
        s->dirs[i] = NULL;
        s->positions[i] = 0;
    }
}

/* Walk from an owned root descriptor without following symlinks. Descriptor-
 * relative operations also remain confined if host paths change concurrently. */
static int parent_fd(SolarOSHostFS *s, const char *path, char *leaf)
{
    char copy[SOLAR_OS_QEMU_HOSTFS_PATH_SIZE], *save, *part;
    if (strnlen(path, sizeof(copy)) == sizeof(copy)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    pstrcpy(copy, sizeof(copy), path);
    int parent = dup(s->root_fd);
    if (parent < 0) return -1;
    pstrcpy(leaf, SOLAR_OS_QEMU_HOSTFS_PATH_SIZE, ".");
    for (part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (!strcmp(part, ".")) continue;
        if (!strcmp(part, "..")) { errno = EACCES; goto fail; }
        if (strcmp(leaf, ".")) {
            int next = openat(parent, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (next < 0) goto fail;
            close(parent);
            parent = next;
        }
        pstrcpy(leaf, SOLAR_OS_QEMU_HOSTFS_PATH_SIZE, part);
    }
    return parent;
fail:
    {
        int saved = errno;
        close(parent);
        errno = saved;
        return -1;
    }
}

static int open_path(SolarOSHostFS *s, const char *path, int flags, mode_t mode)
{
    char leaf[SOLAR_OS_QEMU_HOSTFS_PATH_SIZE];
    int parent = parent_fd(s, path, leaf);
    if (parent < 0) return -1;
    int fd = openat(parent, leaf, flags | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, mode);
    int saved = errno;
    close(parent);
    errno = saved;
    return fd;
}

static uint32_t file_type(mode_t mode)
{
    return S_ISREG(mode) ? SOLAR_OS_QEMU_HOSTFS_TYPE_FILE :
        S_ISDIR(mode) ? SOLAR_OS_QEMU_HOSTFS_TYPE_DIR : SOLAR_OS_QEMU_HOSTFS_TYPE_OTHER;
}

static void encode_stat(SolarOSHostFS *s, const struct stat *st)
{
    memset(s->data, 0, 40);
    stl_le_p(s->data, file_type(st->st_mode));
    stl_le_p(s->data + 4, st->st_mode & 0777);
    stq_le_p(s->data + 8, st->st_size);
    stq_le_p(s->data + 16, st->st_mtime);
    stq_le_p(s->data + 24, st->st_atime);
    stq_le_p(s->data + 32, st->st_ctime);
}

static struct dirent *next_entry(DIR *dir)
{
    struct dirent *entry;
    do {
        errno = 0;
        entry = readdir(dir);
    } while (entry && (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")));
    return entry;
}

static int execute(SolarOSHostFS *s, uint32_t command)
{
    unsigned id = s->args[0];
    struct stat st;
    if (s->root_fd < 0) { errno = ENOENT; return -1; }
    switch (command) {
    case SOLAR_OS_QEMU_HOSTFS_RESET:
        close_handles(s);
        return 0;
    case SOLAR_OS_QEMU_HOSTFS_CLOSE:
    case SOLAR_OS_QEMU_HOSTFS_READ:
    case SOLAR_OS_QEMU_HOSTFS_WRITE:
    case SOLAR_OS_QEMU_HOSTFS_SEEK:
    case SOLAR_OS_QEMU_HOSTFS_FSYNC:
    case SOLAR_OS_QEMU_HOSTFS_FSTAT:
    case SOLAR_OS_QEMU_HOSTFS_TRUNCATE: {
        if (!id || id > G_N_ELEMENTS(s->files) || s->files[id - 1] < 0) {
            errno = EBADF; return -1;
        }
        int fd = s->files[id - 1];
        int64_t offset = (int64_t)(((uint64_t)s->args[2] << 32) | s->args[1]);
        switch (command) {
        case SOLAR_OS_QEMU_HOSTFS_CLOSE:
            s->files[id - 1] = -1;
            return close(fd);
        case SOLAR_OS_QEMU_HOSTFS_READ:
        case SOLAR_OS_QEMU_HOSTFS_WRITE:
            if (s->args[1] > sizeof(s->data)) { errno = EINVAL; return -1; }
            return command == SOLAR_OS_QEMU_HOSTFS_READ ?
                read(fd, s->data, s->args[1]) : write(fd, s->data, s->args[1]);
        case SOLAR_OS_QEMU_HOSTFS_FSYNC: return fsync(fd);
        case SOLAR_OS_QEMU_HOSTFS_TRUNCATE: return ftruncate(fd, offset);
        case SOLAR_OS_QEMU_HOSTFS_SEEK: {
            if (s->args[3] > 2) { errno = EINVAL; return -1; }
            int whence[] = {SEEK_SET, SEEK_CUR, SEEK_END};
            off_t pos = lseek(fd, offset, whence[s->args[3]]);
            if (pos < 0) return -1;
            s->args[0] = (uint64_t)pos;
            s->args[1] = (uint64_t)pos >> 32;
            return 0;
        }
        default:
            if (fstat(fd, &st)) return -1;
            encode_stat(s, &st);
            return 0;
        }
    }
    case SOLAR_OS_QEMU_HOSTFS_READDIR:
    case SOLAR_OS_QEMU_HOSTFS_CLOSEDIR:
    case SOLAR_OS_QEMU_HOSTFS_TELLDIR:
    case SOLAR_OS_QEMU_HOSTFS_SEEKDIR: {
        if (!id || id > G_N_ELEMENTS(s->dirs) || !s->dirs[id - 1]) {
            errno = EBADF; return -1;
        }
        DIR *dir = s->dirs[id - 1];
        if (command == SOLAR_OS_QEMU_HOSTFS_CLOSEDIR) {
            s->dirs[id - 1] = NULL;
            return closedir(dir);
        }
        if (command == SOLAR_OS_QEMU_HOSTFS_TELLDIR) return s->positions[id - 1];
        if (command == SOLAR_OS_QEMU_HOSTFS_SEEKDIR) {
            rewinddir(dir);
            s->positions[id - 1] = 0;
            while (s->positions[id - 1] < s->args[1] && next_entry(dir)) s->positions[id - 1]++;
            return errno ? -1 : 0;
        }
        struct dirent *entry = next_entry(dir);
        if (!entry) return errno ? -1 : 0;
        s->positions[id - 1]++;
        stl_le_p(s->data, entry->d_type == DT_DIR ? SOLAR_OS_QEMU_HOSTFS_TYPE_DIR :
            entry->d_type == DT_REG ? SOLAR_OS_QEMU_HOSTFS_TYPE_FILE : SOLAR_OS_QEMU_HOSTFS_TYPE_OTHER);
        pstrcpy((char *)s->data + 4, sizeof(s->data) - 4, entry->d_name);
        return 1;
    }
    case SOLAR_OS_QEMU_HOSTFS_USAGE: {
        struct statvfs usage;
        if (fstatvfs(s->root_fd, &usage)) return -1;
        uint64_t total = (uint64_t)usage.f_blocks * usage.f_frsize;
        uint64_t free = (uint64_t)usage.f_bfree * usage.f_frsize;
        stq_le_p(s->data, total);
        stq_le_p(s->data + 8, total - MIN(total, free));
        stq_le_p(s->data + 16, (uint64_t)usage.f_bavail * usage.f_frsize);
        return 0;
    }
    default: break;
    }
    if (!memchr(s->data, 0, sizeof(s->data))) { errno = ENAMETOOLONG; return -1; }
    const char *path = (const char *)s->data;
    if (command == SOLAR_OS_QEMU_HOSTFS_OPEN || command == SOLAR_OS_QEMU_HOSTFS_OPENDIR) {
        unsigned slot, limit = command == SOLAR_OS_QEMU_HOSTFS_OPEN ?
            G_N_ELEMENTS(s->files) : G_N_ELEMENTS(s->dirs);
        for (slot = 0; slot < limit; slot++) {
            if (command == SOLAR_OS_QEMU_HOSTFS_OPEN ? s->files[slot] < 0 : !s->dirs[slot]) break;
        }
        if (slot == limit) { errno = EMFILE; return -1; }
        int flags = O_RDONLY;
        if (command == SOLAR_OS_QEMU_HOSTFS_OPENDIR) flags |= O_DIRECTORY;
        else {
            if ((id & 3U) == 3U || id & ~63U) { errno = EINVAL; return -1; }
            flags = (id & SOLAR_OS_QEMU_HOSTFS_O_RDWR) ? O_RDWR :
                (id & SOLAR_OS_QEMU_HOSTFS_O_WRITE) ? O_WRONLY : O_RDONLY;
            if (id & SOLAR_OS_QEMU_HOSTFS_O_CREATE) flags |= O_CREAT;
            if (id & SOLAR_OS_QEMU_HOSTFS_O_TRUNCATE) flags |= O_TRUNC;
            if (id & SOLAR_OS_QEMU_HOSTFS_O_APPEND) flags |= O_APPEND;
            if (id & SOLAR_OS_QEMU_HOSTFS_O_EXCLUSIVE) flags |= O_EXCL;
        }
        int fd = open_path(s, path, flags, s->args[1] & 0777);
        if (fd < 0) return -1;
        if (command == SOLAR_OS_QEMU_HOSTFS_OPEN) {
            if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
                close(fd); errno = EPERM; return -1;
            }
            s->files[slot] = fd;
        } else {
            s->dirs[slot] = fdopendir(fd);
            if (!s->dirs[slot]) { int saved = errno; close(fd); errno = saved; return -1; }
            s->positions[slot] = 0;
        }
        return slot + 1;
    }
    char leaf[SOLAR_OS_QEMU_HOSTFS_PATH_SIZE];
    int parent = parent_fd(s, path, leaf);
    if (parent < 0) return -1;
    int ret = -1;
    switch (command) {
    case SOLAR_OS_QEMU_HOSTFS_STAT:
        ret = fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW);
        if (!ret && S_ISLNK(st.st_mode)) { errno = ELOOP; ret = -1; }
        if (!ret) encode_stat(s, &st);
        break;
    case SOLAR_OS_QEMU_HOSTFS_UNLINK: ret = unlinkat(parent, leaf, 0); break;
    case SOLAR_OS_QEMU_HOSTFS_RMDIR: ret = unlinkat(parent, leaf, AT_REMOVEDIR); break;
    case SOLAR_OS_QEMU_HOSTFS_MKDIR: ret = mkdirat(parent, leaf, s->args[0] & 0777); break;
    case SOLAR_OS_QEMU_HOSTFS_RENAME: {
        unsigned offset = strlen(path) + 1;
        if (offset >= sizeof(s->data) || !memchr(s->data + offset, 0, sizeof(s->data) - offset)) {
            errno = EINVAL; break;
        }
        char dest[SOLAR_OS_QEMU_HOSTFS_PATH_SIZE];
        int other = parent_fd(s, (const char *)s->data + offset, dest);
        if (other >= 0) {
            ret = renameat(parent, leaf, other, dest);
            int saved = errno; close(other); errno = saved;
        }
        break;
    }
    case SOLAR_OS_QEMU_HOSTFS_UTIME: {
        struct timespec times[2] = {
            {.tv_sec = (int64_t)(((uint64_t)s->args[1] << 32) | s->args[0])},
            {.tv_sec = (int64_t)(((uint64_t)s->args[3] << 32) | s->args[2])},
        };
        ret = utimensat(parent, leaf, times, AT_SYMLINK_NOFOLLOW);
        break;
    }
    default: errno = ENOSYS; break;
    }
    int saved = errno;
    close(parent);
    errno = saved;
    return ret;
}

static uint64_t hostfs_read(void *opaque, hwaddr offset, unsigned size)
{
    SolarOSHostFS *s = opaque;
    if (offset >= SOLAR_OS_QEMU_HOSTFS_REG_DATA &&
        offset + size <= SOLAR_OS_QEMU_HOSTFS_REG_DATA + sizeof(s->data)) {
        uint64_t value = 0;
        for (unsigned i = 0; i < size; i++) value |= (uint64_t)s->data[offset - SOLAR_OS_QEMU_HOSTFS_REG_DATA + i] << (i * 8);
        return value;
    }
    if (size != 4) return 0;
    if (offset >= SOLAR_OS_QEMU_HOSTFS_REG_ARG0 && offset < SOLAR_OS_QEMU_HOSTFS_REG_ARG0 + 16)
        return s->args[(offset - SOLAR_OS_QEMU_HOSTFS_REG_ARG0) / 4];
    switch (offset) {
    case SOLAR_OS_QEMU_HOSTFS_REG_MAGIC: return SOLAR_OS_QEMU_HOSTFS_MAGIC;
    case SOLAR_OS_QEMU_HOSTFS_REG_VERSION: return SOLAR_OS_QEMU_HOSTFS_VERSION;
    case SOLAR_OS_QEMU_HOSTFS_REG_READY: return s->root_fd >= 0;
    case SOLAR_OS_QEMU_HOSTFS_REG_RESULT: return s->result;
    case SOLAR_OS_QEMU_HOSTFS_REG_ERROR: return s->error;
    default: return 0;
    }
}

static void hostfs_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    SolarOSHostFS *s = opaque;
    if (offset >= SOLAR_OS_QEMU_HOSTFS_REG_DATA &&
        offset + size <= SOLAR_OS_QEMU_HOSTFS_REG_DATA + sizeof(s->data)) {
        for (unsigned i = 0; i < size; i++) s->data[offset - SOLAR_OS_QEMU_HOSTFS_REG_DATA + i] = value >> (i * 8);
    } else if (size == 4 && offset >= SOLAR_OS_QEMU_HOSTFS_REG_ARG0 && offset < SOLAR_OS_QEMU_HOSTFS_REG_ARG0 + 16) {
        s->args[(offset - SOLAR_OS_QEMU_HOSTFS_REG_ARG0) / 4] = value;
    } else if (size == 4 && offset == SOLAR_OS_QEMU_HOSTFS_REG_COMMAND) {
        errno = 0;
        int ret = execute(s, value);
        s->result = ret;
        s->error = ret < 0 ? wire_error(errno) : 0;
    }
}

static const MemoryRegionOps hostfs_ops = {
    .read = hostfs_read, .write = hostfs_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1, .valid.max_access_size = 4,
};

static void hostfs_realize(DeviceState *dev, Error **errp)
{
    SolarOSHostFS *s = SOLAR_OS_HOSTFS(dev);
    if (s->root && s->root[0]) {
        s->root_fd = open(s->root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (s->root_fd < 0) error_setg_errno(errp, errno, "Cannot open SolarOS host storage '%s'", s->root);
    }
}

static void hostfs_unrealize(DeviceState *dev)
{
    SolarOSHostFS *s = SOLAR_OS_HOSTFS(dev);
    close_handles(s);
    if (s->root_fd >= 0) close(s->root_fd);
    s->root_fd = -1;
}

static void hostfs_reset(Object *obj, ResetType type)
{
    SolarOSHostFS *s = SOLAR_OS_HOSTFS(obj);
    close_handles(s);
    memset(s->args, 0, sizeof(s->args));
    memset(s->data, 0, sizeof(s->data));
    s->result = s->error = 0;
}

static void hostfs_init(Object *obj)
{
    SolarOSHostFS *s = SOLAR_OS_HOSTFS(obj);
    s->root_fd = -1;
    for (unsigned i = 0; i < G_N_ELEMENTS(s->files); i++) s->files[i] = -1;
    memory_region_init_io(&s->mmio, obj, &hostfs_ops, s, TYPE_SOLAR_OS_HOSTFS, 0x2000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static Property hostfs_properties[] = {
    DEFINE_PROP_STRING("root", SolarOSHostFS, root), DEFINE_PROP_END_OF_LIST(),
};

static void hostfs_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = hostfs_realize;
    dc->unrealize = hostfs_unrealize;
    device_class_set_props(dc, hostfs_properties);
    RESETTABLE_CLASS(klass)->phases.hold = hostfs_reset;
}

static const TypeInfo hostfs_type = {
    .name = TYPE_SOLAR_OS_HOSTFS, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SolarOSHostFS), .instance_init = hostfs_init,
    .class_init = hostfs_class_init,
};

static void hostfs_register(void) { type_register_static(&hostfs_type); }
type_init(hostfs_register)
