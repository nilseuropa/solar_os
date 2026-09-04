#include "solar_os_ramfs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_board_caps.h"
#include "solar_os_memory.h"

#define RAMFS_MAX_OPEN_FILES 16
#define RAMFS_MIN_QUOTA_BYTES 1024U
#define RAMFS_ALIGNMENT sizeof(void *)

typedef struct ramfs_node ramfs_node_t;
typedef struct ramfs_block ramfs_block_t;

struct ramfs_block {
    size_t size;
    bool free;
    ramfs_block_t *prev;
    ramfs_block_t *next;
};

struct ramfs_node {
    char *name;
    bool is_dir;
    ramfs_node_t *parent;
    ramfs_node_t *children;
    ramfs_node_t *next;
    uint8_t *data;
    size_t size;
    size_t capacity;
    time_t mtime;
};

typedef struct {
    bool active;
    ramfs_node_t *node;
    size_t offset;
    int flags;
} ramfs_file_t;

typedef struct {
    bool active;
    char mount_point[SOLAR_OS_RAMFS_MOUNT_POINT_MAX];
    size_t quota_bytes;
    size_t arena_bytes;
    uint8_t *arena;
    ramfs_block_t *blocks;
    size_t file_count;
    size_t dir_count;
    ramfs_node_t root;
    ramfs_file_t files[RAMFS_MAX_OPEN_FILES];
    SemaphoreHandle_t lock;
} ramfs_mount_t;

typedef struct {
    DIR dir;
    ramfs_mount_t *mount;
    ramfs_node_t *dir_node;
    ramfs_node_t *next;
    long pos;
    struct dirent entry;
} ramfs_dir_t;

static ramfs_mount_t mounts[SOLAR_OS_RAMFS_MAX_MOUNTS];

static bool ramfs_path_is_on_mount(const char *path, const char *mount_point)
{
    if (path == NULL || mount_point == NULL || mount_point[0] == '\0') {
        return false;
    }
    if (strcmp(mount_point, "/") == 0) {
        return path[0] == '/';
    }

    const size_t len = strlen(mount_point);
    return strncmp(path, mount_point, len) == 0 &&
        (path[len] == '\0' || path[len] == '/');
}

static const char *ramfs_vfs_base_path(const char *mount_point)
{
    return mount_point != NULL && strcmp(mount_point, "/") == 0 ? "" : mount_point;
}

static void ramfs_lock(ramfs_mount_t *mount)
{
    if (mount != NULL && mount->lock != NULL) {
        (void)xSemaphoreTake(mount->lock, portMAX_DELAY);
    }
}

static void ramfs_unlock(ramfs_mount_t *mount)
{
    if (mount != NULL && mount->lock != NULL) {
        (void)xSemaphoreGive(mount->lock);
    }
}

static bool ramfs_align_size(size_t size, size_t *aligned)
{
    if (aligned == NULL) {
        return false;
    }

    const size_t mask = RAMFS_ALIGNMENT - 1U;
    if (size > SIZE_MAX - mask) {
        return false;
    }
    *aligned = (size + mask) & ~mask;
    return true;
}

static uint8_t *ramfs_block_payload(ramfs_block_t *block)
{
    return (uint8_t *)block + sizeof(*block);
}

static ramfs_block_t *ramfs_payload_block(void *ptr)
{
    return (ramfs_block_t *)((uint8_t *)ptr - sizeof(ramfs_block_t));
}

static bool ramfs_ptr_in_arena(const ramfs_mount_t *mount, const void *ptr)
{
    if (mount == NULL || mount->arena == NULL || ptr == NULL) {
        return false;
    }

    const uint8_t *p = (const uint8_t *)ptr;
    return p >= mount->arena + sizeof(ramfs_block_t) &&
        p < mount->arena + mount->arena_bytes;
}

static void ramfs_split_block(ramfs_block_t *block, size_t size)
{
    if (block == NULL || block->size <= size) {
        return;
    }

    const size_t remaining = block->size - size;
    if (remaining <= sizeof(ramfs_block_t) + RAMFS_ALIGNMENT) {
        return;
    }

    ramfs_block_t *next = (ramfs_block_t *)(ramfs_block_payload(block) + size);
    next->size = remaining - sizeof(ramfs_block_t);
    next->free = true;
    next->prev = block;
    next->next = block->next;
    if (next->next != NULL) {
        next->next->prev = next;
    }

    block->size = size;
    block->next = next;
}

static void ramfs_merge_next(ramfs_block_t *block)
{
    if (block == NULL || block->next == NULL || !block->next->free) {
        return;
    }

    ramfs_block_t *next = block->next;
    block->size += sizeof(*next) + next->size;
    block->next = next->next;
    if (block->next != NULL) {
        block->next->prev = block;
    }
}

static void ramfs_coalesce_block(ramfs_block_t *block)
{
    if (block == NULL) {
        return;
    }

    ramfs_merge_next(block);
    if (block->prev != NULL && block->prev->free) {
        ramfs_merge_next(block->prev);
    }
}

static size_t ramfs_free_payload_bytes(const ramfs_mount_t *mount)
{
    if (mount == NULL) {
        return 0;
    }

    size_t bytes = 0;
    for (const ramfs_block_t *block = mount->blocks; block != NULL; block = block->next) {
        if (block->free) {
            bytes += block->size;
        }
    }
    return bytes;
}

static void ramfs_get_space_locked(const ramfs_mount_t *mount,
                                   uint64_t *total_bytes,
                                   uint64_t *used_bytes,
                                   uint64_t *free_bytes)
{
    const uint64_t total = mount == NULL ? 0 : (uint64_t)mount->quota_bytes;
    uint64_t free_space = (uint64_t)ramfs_free_payload_bytes(mount);
    if (free_space > total) {
        free_space = total;
    }

    if (total_bytes != NULL) {
        *total_bytes = total;
    }
    if (used_bytes != NULL) {
        *used_bytes = total - free_space;
    }
    if (free_bytes != NULL) {
        *free_bytes = free_space;
    }
}

static void *ramfs_alloc(ramfs_mount_t *mount, size_t size)
{
    if (mount == NULL || size == 0) {
        errno = EINVAL;
        return NULL;
    }

    size_t aligned = 0;
    if (!ramfs_align_size(size, &aligned)) {
        errno = ENOSPC;
        return NULL;
    }

    for (ramfs_block_t *block = mount->blocks; block != NULL; block = block->next) {
        if (!block->free || block->size < aligned) {
            continue;
        }

        ramfs_split_block(block, aligned);
        block->free = false;
        return ramfs_block_payload(block);
    }

    errno = ENOSPC;
    return NULL;
}

static void ramfs_free(ramfs_mount_t *mount, void *ptr, size_t size)
{
    (void)size;

    if (ptr == NULL) {
        return;
    }
    if (!ramfs_ptr_in_arena(mount, ptr)) {
        return;
    }

    ramfs_block_t *block = ramfs_payload_block(ptr);
    block->free = true;
    ramfs_coalesce_block(block);
}

static void *ramfs_realloc(ramfs_mount_t *mount, void *ptr, size_t old_size, size_t new_size)
{
    if (mount == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (new_size == 0) {
        ramfs_free(mount, ptr, old_size);
        return NULL;
    }
    if (ptr == NULL) {
        return ramfs_alloc(mount, new_size);
    }

    size_t aligned = 0;
    if (!ramfs_align_size(new_size, &aligned) || !ramfs_ptr_in_arena(mount, ptr)) {
        errno = EINVAL;
        return NULL;
    }

    ramfs_block_t *block = ramfs_payload_block(ptr);
    if (block->size >= aligned) {
        ramfs_split_block(block, aligned);
        return ptr;
    }

    if (block->next != NULL && block->next->free &&
        block->size + sizeof(*block) + block->next->size >= aligned) {
        ramfs_merge_next(block);
        ramfs_split_block(block, aligned);
        return ptr;
    }

    void *next = ramfs_alloc(mount, new_size);
    if (next == NULL) {
        return NULL;
    }
    size_t copy_size = old_size;
    if (copy_size > block->size) {
        copy_size = block->size;
    }
    if (copy_size > new_size) {
        copy_size = new_size;
    }
    memcpy(next, ptr, copy_size);
    ramfs_free(mount, ptr, old_size);
    return next;
}

static ramfs_node_t *ramfs_find_child(ramfs_node_t *parent, const char *name)
{
    if (parent == NULL || name == NULL) {
        return NULL;
    }

    for (ramfs_node_t *child = parent->children; child != NULL; child = child->next) {
        if (strcmp(child->name, name) == 0) {
            return child;
        }
    }
    return NULL;
}

static ramfs_node_t *ramfs_alloc_node(ramfs_mount_t *mount,
                                      ramfs_node_t *parent,
                                      const char *name,
                                      bool is_dir)
{
    const size_t name_len = strlen(name);
    ramfs_node_t *node = ramfs_alloc(mount, sizeof(*node));
    if (node == NULL) {
        return NULL;
    }

    char *copy = ramfs_alloc(mount, name_len + 1);
    if (copy == NULL) {
        ramfs_free(mount, node, sizeof(*node));
        return NULL;
    }

    memset(node, 0, sizeof(*node));
    memcpy(copy, name, name_len + 1);
    node->name = copy;
    node->is_dir = is_dir;
    node->parent = parent;
    node->mtime = time(NULL);
    node->next = parent->children;
    parent->children = node;
    if (is_dir) {
        mount->dir_count++;
    } else {
        mount->file_count++;
    }
    return node;
}

static void ramfs_free_tree(ramfs_mount_t *mount, ramfs_node_t *node)
{
    if (mount == NULL || node == NULL) {
        return;
    }

    ramfs_node_t *child = node->children;
    while (child != NULL) {
        ramfs_node_t *next = child->next;
        ramfs_free_tree(mount, child);
        child = next;
    }

    if (node != &mount->root) {
        if (node->is_dir) {
            mount->dir_count = mount->dir_count > 0 ? mount->dir_count - 1 : 0;
        } else {
            mount->file_count = mount->file_count > 0 ? mount->file_count - 1 : 0;
        }
        ramfs_free(mount, node->data, node->capacity);
        ramfs_free(mount, node->name, strlen(node->name) + 1);
        ramfs_free(mount, node, sizeof(*node));
    } else {
        node->children = NULL;
    }
}

static void ramfs_unlink_node(ramfs_node_t *node)
{
    if (node == NULL || node->parent == NULL) {
        return;
    }

    ramfs_node_t **cursor = &node->parent->children;
    while (*cursor != NULL) {
        if (*cursor == node) {
            *cursor = node->next;
            node->next = NULL;
            return;
        }
        cursor = &(*cursor)->next;
    }
}

static bool ramfs_next_segment(const char **cursor, char *segment, size_t segment_len)
{
    if (cursor == NULL || *cursor == NULL || segment == NULL || segment_len == 0) {
        return false;
    }

    const char *p = *cursor;
    while (*p == '/') {
        p++;
    }
    if (*p == '\0') {
        *cursor = p;
        return false;
    }

    const char *start = p;
    while (*p != '\0' && *p != '/') {
        p++;
    }
    const size_t len = (size_t)(p - start);
    if (len >= segment_len) {
        errno = ENAMETOOLONG;
        return false;
    }
    memcpy(segment, start, len);
    segment[len] = '\0';
    *cursor = p;
    return true;
}

static ramfs_node_t *ramfs_find_node(ramfs_mount_t *mount, const char *path)
{
    if (mount == NULL || path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }

    ramfs_node_t *node = &mount->root;
    const char *cursor = path;
    char segment[NAME_MAX + 1];
    while (ramfs_next_segment(&cursor, segment, sizeof(segment))) {
        if (strcmp(segment, ".") == 0) {
            continue;
        }
        if (strcmp(segment, "..") == 0) {
            if (node->parent != NULL) {
                node = node->parent;
            }
            continue;
        }
        if (!node->is_dir) {
            errno = ENOTDIR;
            return NULL;
        }
        node = ramfs_find_child(node, segment);
        if (node == NULL) {
            errno = ENOENT;
            return NULL;
        }
    }
    return node;
}

static esp_err_t ramfs_resolve_parent(ramfs_mount_t *mount,
                                      const char *path,
                                      ramfs_node_t **parent,
                                      char *leaf,
                                      size_t leaf_len)
{
    if (mount == NULL || path == NULL || parent == NULL || leaf == NULL || leaf_len == 0) {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }

    ramfs_node_t *node = &mount->root;
    const char *cursor = path;
    char segment[NAME_MAX + 1];
    char last[NAME_MAX + 1] = {0};

    while (ramfs_next_segment(&cursor, segment, sizeof(segment))) {
        if (strcmp(segment, ".") == 0) {
            continue;
        }
        if (strcmp(segment, "..") == 0) {
            if (node->parent != NULL) {
                node = node->parent;
            }
            last[0] = '\0';
            continue;
        }

        const char *peek = cursor;
        char next[NAME_MAX + 1];
        bool has_next = false;
        while (ramfs_next_segment(&peek, next, sizeof(next))) {
            if (strcmp(next, ".") != 0) {
                has_next = true;
                break;
            }
        }

        if (!has_next) {
            strlcpy(last, segment, sizeof(last));
            break;
        }

        node = ramfs_find_child(node, segment);
        if (node == NULL) {
            errno = ENOENT;
            return ESP_ERR_NOT_FOUND;
        }
        if (!node->is_dir) {
            errno = ENOTDIR;
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (last[0] == '\0') {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }
    if (!node->is_dir) {
        errno = ENOTDIR;
        return ESP_ERR_INVALID_STATE;
    }
    if (strlcpy(leaf, last, leaf_len) >= leaf_len) {
        errno = ENAMETOOLONG;
        return ESP_ERR_INVALID_SIZE;
    }
    *parent = node;
    return ESP_OK;
}

static bool ramfs_node_is_open(const ramfs_mount_t *mount, const ramfs_node_t *node)
{
    if (mount == NULL || node == NULL) {
        return false;
    }

    for (size_t i = 0; i < RAMFS_MAX_OPEN_FILES; i++) {
        if (mount->files[i].active && mount->files[i].node == node) {
            return true;
        }
    }
    return false;
}

static int ramfs_allocate_fd(ramfs_mount_t *mount, ramfs_node_t *node, int flags)
{
    for (size_t i = 0; i < RAMFS_MAX_OPEN_FILES; i++) {
        if (!mount->files[i].active) {
            mount->files[i].active = true;
            mount->files[i].node = node;
            mount->files[i].offset = (flags & O_APPEND) ? node->size : 0;
            mount->files[i].flags = flags;
            return (int)i;
        }
    }
    errno = EMFILE;
    return -1;
}

static ramfs_file_t *ramfs_get_file(ramfs_mount_t *mount, int fd)
{
    if (mount == NULL || fd < 0 || fd >= RAMFS_MAX_OPEN_FILES || !mount->files[fd].active) {
        errno = EBADF;
        return NULL;
    }
    return &mount->files[fd];
}

static int ramfs_resize_node(ramfs_mount_t *mount, ramfs_node_t *node, size_t size)
{
    if (node == NULL || node->is_dir) {
        errno = EISDIR;
        return -1;
    }

    if (size == 0) {
        ramfs_free(mount, node->data, node->capacity);
        node->data = NULL;
        node->size = 0;
        node->capacity = 0;
        node->mtime = time(NULL);
        return 0;
    }

    uint8_t *next = ramfs_realloc(mount, node->data, node->capacity, size);
    if (next == NULL) {
        return -1;
    }
    if (size > node->capacity) {
        memset(next + node->capacity, 0, size - node->capacity);
    }
    node->data = next;
    node->capacity = size;
    node->size = size;
    node->mtime = time(NULL);
    return 0;
}

static int ramfs_vfs_open(void *ctx, const char *path, int flags, int mode)
{
    (void)mode;
    ramfs_mount_t *mount = (ramfs_mount_t *)ctx;
    int fd = -1;

    ramfs_lock(mount);
    ramfs_node_t *node = ramfs_find_node(mount, path);
    if (node == NULL) {
        if (!(flags & O_CREAT)) {
            goto out;
        }

        ramfs_node_t *parent = NULL;
        char leaf[NAME_MAX + 1];
        if (ramfs_resolve_parent(mount, path, &parent, leaf, sizeof(leaf)) != ESP_OK) {
            goto out;
        }
        if (ramfs_find_child(parent, leaf) != NULL) {
            errno = EEXIST;
            goto out;
        }
        node = ramfs_alloc_node(mount, parent, leaf, false);
        if (node == NULL) {
            goto out;
        }
    } else if ((flags & O_CREAT) && (flags & O_EXCL)) {
        errno = EEXIST;
        goto out;
    }

    if (node->is_dir) {
        errno = EISDIR;
        goto out;
    }

    const int accmode = flags & O_ACCMODE;
    if ((flags & O_TRUNC) && accmode != O_RDONLY && ramfs_resize_node(mount, node, 0) != 0) {
        goto out;
    }

    fd = ramfs_allocate_fd(mount, node, flags);

out:
    ramfs_unlock(mount);
    return fd;
}

static int ramfs_vfs_close(void *ctx, int fd)
{
    ramfs_mount_t *mount = (ramfs_mount_t *)ctx;
    int ret = -1;

    ramfs_lock(mount);
    ramfs_file_t *file = ramfs_get_file(mount, fd);
    if (file != NULL) {
        memset(file, 0, sizeof(*file));
        ret = 0;
    }
    ramfs_unlock(mount);
    return ret;
}

static ssize_t ramfs_vfs_read(void *ctx, int fd, void *dst, size_t size)
{
    ramfs_mount_t *mount = (ramfs_mount_t *)ctx;
    ssize_t ret = -1;

    if (dst == NULL && size > 0) {
        errno = EINVAL;
        return -1;
    }

    ramfs_lock(mount);
    ramfs_file_t *file = ramfs_get_file(mount, fd);
    if (file == NULL) {
        goto out;
    }
    if ((file->flags & O_ACCMODE) == O_WRONLY) {
        errno = EBADF;
        goto out;
    }

    ramfs_node_t *node = file->node;
    if (file->offset >= node->size) {
        ret = 0;
        goto out;
    }
    size_t remaining = node->size - file->offset;
    if (size > remaining) {
        size = remaining;
    }
    memcpy(dst, node->data + file->offset, size);
    file->offset += size;
    ret = (ssize_t)size;

out:
    ramfs_unlock(mount);
    return ret;
}

static ssize_t ramfs_vfs_write(void *ctx, int fd, const void *data, size_t size)
{
    ramfs_mount_t *mount = (ramfs_mount_t *)ctx;
    ssize_t ret = -1;

    if (data == NULL && size > 0) {
        errno = EINVAL;
        return -1;
    }

    ramfs_lock(mount);
    ramfs_file_t *file = ramfs_get_file(mount, fd);
    if (file == NULL) {
        goto out;
    }
    if ((file->flags & O_ACCMODE) == O_RDONLY) {
        errno = EBADF;
        goto out;
    }

    ramfs_node_t *node = file->node;
    if (file->flags & O_APPEND) {
        file->offset = node->size;
    }
    if (size > SIZE_MAX - file->offset) {
        errno = EFBIG;
        goto out;
    }
    const size_t end = file->offset + size;
    if (end > node->capacity && ramfs_resize_node(mount, node, end) != 0) {
        goto out;
    }
    memcpy(node->data + file->offset, data, size);
    file->offset = end;
    if (end > node->size) {
        node->size = end;
    }
    node->mtime = time(NULL);
    ret = (ssize_t)size;

out:
    ramfs_unlock(mount);
    return ret;
}

static off_t ramfs_vfs_lseek(void *ctx, int fd, off_t offset, int mode)
{
    ramfs_mount_t *mount = (ramfs_mount_t *)ctx;
    off_t ret = -1;

    ramfs_lock(mount);
    ramfs_file_t *file = ramfs_get_file(mount, fd);
    if (file == NULL) {
        goto out;
    }

    int64_t next = 0;
    switch (mode) {
    case SEEK_SET:
        next = offset;
        break;
    case SEEK_CUR:
        next = (int64_t)file->offset + offset;
        break;
    case SEEK_END:
        next = (int64_t)file->node->size + offset;
        break;
    default:
        errno = EINVAL;
        goto out;
    }
    if (next < 0 || next > SIZE_MAX) {
        errno = EINVAL;
        goto out;
    }
    file->offset = (size_t)next;
    ret = (off_t)file->offset;

out:
    ramfs_unlock(mount);
    return ret;
}

static int ramfs_fill_stat(const ramfs_node_t *node, struct stat *st)
{
    if (node == NULL || st == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(st, 0, sizeof(*st));
    st->st_mode = node->is_dir ? (S_IFDIR | 0777) : (S_IFREG | 0666);
    st->st_size = node->is_dir ? 0 : (off_t)node->size;
    st->st_mtime = node->mtime;
    st->st_ctime = node->mtime;
    st->st_atime = node->mtime;
    return 0;
}

static int ramfs_vfs_fstat(void *ctx, int fd, struct stat *st)
{
    ramfs_mount_t *mount = (ramfs_mount_t *)ctx;
    int ret = -1;

    ramfs_lock(mount);
    ramfs_file_t *file = ramfs_get_file(mount, fd);
    if (file != NULL) {
        ret = ramfs_fill_stat(file->node, st);
    }
    ramfs_unlock(mount);
    return ret;
}

static int ramfs_vfs_stat(void *ctx, const char *path, struct stat *st)
{
    ramfs_mount_t *mount = (ramfs_mount_t *)ctx;
    int ret = -1;

    ramfs_lock(mount);
    ramfs_node_t *node = ramfs_find_node(mount, path);
    if (node != NULL) {
        ret = ramfs_fill_stat(node, st);
    }
    ramfs_unlock(mount);
    return ret;
}

static int ramfs_vfs_unlink(void *ctx, const char *path)
{
    ramfs_mount_t *mount = (ramfs_mount_t *)ctx;
    int ret = -1;

    ramfs_lock(mount);
    ramfs_node_t *node = ramfs_find_node(mount, path);
    if (node == NULL) {
        goto out;
    }
    if (node->is_dir) {
        errno = EISDIR;
        goto out;
    }
    if (ramfs_node_is_open(mount, node)) {
        errno = EBUSY;
        goto out;
    }
    ramfs_unlink_node(node);
    ramfs_free_tree(mount, node);
    ret = 0;

out:
    ramfs_unlock(mount);
    return ret;
}

static int ramfs_vfs_rename(void *ctx, const char *src, const char *dst)
{
    ramfs_mount_t *mount = (ramfs_mount_t *)ctx;
    int ret = -1;

    ramfs_lock(mount);
    ramfs_node_t *node = ramfs_find_node(mount, src);
    if (node == NULL) {
        goto out;
    }
    if (node == &mount->root) {
        errno = EINVAL;
        goto out;
    }

    ramfs_node_t *new_parent = NULL;
    char leaf[NAME_MAX + 1];
    if (ramfs_resolve_parent(mount, dst, &new_parent, leaf, sizeof(leaf)) != ESP_OK) {
        goto out;
    }
    if (ramfs_find_child(new_parent, leaf) != NULL) {
        errno = EEXIST;
        goto out;
    }

    char *new_name = ramfs_alloc(mount, strlen(leaf) + 1);
    if (new_name == NULL) {
        goto out;
    }
    memcpy(new_name, leaf, strlen(leaf) + 1);

    const size_t old_name_len = strlen(node->name) + 1;
    ramfs_unlink_node(node);
    ramfs_free(mount, node->name, old_name_len);
    node->name = new_name;
    node->parent = new_parent;
    node->next = new_parent->children;
    new_parent->children = node;
    node->mtime = time(NULL);
    ret = 0;

out:
    ramfs_unlock(mount);
    return ret;
}

static DIR *ramfs_vfs_opendir(void *ctx, const char *name)
{
    ramfs_mount_t *mount = (ramfs_mount_t *)ctx;
    ramfs_dir_t *dir = NULL;

    ramfs_lock(mount);
    ramfs_node_t *node = ramfs_find_node(mount, name);
    if (node == NULL) {
        goto out;
    }
    if (!node->is_dir) {
        errno = ENOTDIR;
        goto out;
    }

    dir = solar_os_memory_calloc(1,
                                 sizeof(*dir),
                                 SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                 "ramfs.dir");
    if (dir == NULL) {
        errno = ENOMEM;
        goto out;
    }
    dir->mount = mount;
    dir->dir_node = node;
    dir->next = node->children;

out:
    ramfs_unlock(mount);
    return (DIR *)dir;
}

static struct dirent *ramfs_vfs_readdir(void *ctx, DIR *pdir)
{
    (void)ctx;
    ramfs_dir_t *dir = (ramfs_dir_t *)pdir;
    if (dir == NULL || dir->mount == NULL) {
        errno = EBADF;
        return NULL;
    }

    ramfs_lock(dir->mount);
    ramfs_node_t *node = dir->next;
    if (node == NULL) {
        ramfs_unlock(dir->mount);
        return NULL;
    }
    dir->next = node->next;
    dir->pos++;
    memset(&dir->entry, 0, sizeof(dir->entry));
    strlcpy(dir->entry.d_name, node->name, sizeof(dir->entry.d_name));
#ifdef DT_DIR
    dir->entry.d_type = node->is_dir ? DT_DIR : DT_REG;
#endif
    ramfs_unlock(dir->mount);
    return &dir->entry;
}

static int ramfs_vfs_readdir_r(void *ctx, DIR *pdir, struct dirent *entry, struct dirent **out)
{
    if (entry == NULL || out == NULL) {
        errno = EINVAL;
        return EINVAL;
    }

    struct dirent *next = ramfs_vfs_readdir(ctx, pdir);
    if (next == NULL) {
        *out = NULL;
        return 0;
    }
    memcpy(entry, next, sizeof(*entry));
    *out = entry;
    return 0;
}

static long ramfs_vfs_telldir(void *ctx, DIR *pdir)
{
    (void)ctx;
    ramfs_dir_t *dir = (ramfs_dir_t *)pdir;
    return dir == NULL ? -1 : dir->pos;
}

static void ramfs_vfs_seekdir(void *ctx, DIR *pdir, long offset)
{
    (void)ctx;
    ramfs_dir_t *dir = (ramfs_dir_t *)pdir;
    if (dir == NULL || dir->mount == NULL || offset < 0) {
        return;
    }

    ramfs_lock(dir->mount);
    if (dir->dir_node != NULL) {
        dir->next = dir->dir_node->children;
        dir->pos = 0;
        while (dir->next != NULL && dir->pos < offset) {
            dir->next = dir->next->next;
            dir->pos++;
        }
    }
    ramfs_unlock(dir->mount);
}

static int ramfs_vfs_closedir(void *ctx, DIR *pdir)
{
    (void)ctx;
    if (pdir == NULL) {
        errno = EBADF;
        return -1;
    }
    solar_os_memory_free(pdir);
    return 0;
}

static int ramfs_vfs_mkdir(void *ctx, const char *name, mode_t mode)
{
    (void)mode;
    ramfs_mount_t *mount = (ramfs_mount_t *)ctx;
    int ret = -1;

    ramfs_lock(mount);
    ramfs_node_t *parent = NULL;
    char leaf[NAME_MAX + 1];
    if (ramfs_resolve_parent(mount, name, &parent, leaf, sizeof(leaf)) != ESP_OK) {
        goto out;
    }
    if (ramfs_find_child(parent, leaf) != NULL) {
        errno = EEXIST;
        goto out;
    }
    ret = ramfs_alloc_node(mount, parent, leaf, true) == NULL ? -1 : 0;

out:
    ramfs_unlock(mount);
    return ret;
}

static int ramfs_vfs_rmdir(void *ctx, const char *name)
{
    ramfs_mount_t *mount = (ramfs_mount_t *)ctx;
    int ret = -1;

    ramfs_lock(mount);
    ramfs_node_t *node = ramfs_find_node(mount, name);
    if (node == NULL) {
        goto out;
    }
    if (node == &mount->root) {
        errno = EINVAL;
        goto out;
    }
    if (!node->is_dir) {
        errno = ENOTDIR;
        goto out;
    }
    if (node->children != NULL) {
        errno = ENOTEMPTY;
        goto out;
    }
    ramfs_unlink_node(node);
    ramfs_free_tree(mount, node);
    ret = 0;

out:
    ramfs_unlock(mount);
    return ret;
}

static int ramfs_vfs_access(void *ctx, const char *path, int amode)
{
    (void)amode;
    ramfs_mount_t *mount = (ramfs_mount_t *)ctx;
    int ret = -1;

    ramfs_lock(mount);
    ret = ramfs_find_node(mount, path) == NULL ? -1 : 0;
    ramfs_unlock(mount);
    return ret;
}

static int ramfs_vfs_truncate(void *ctx, const char *path, off_t length)
{
    ramfs_mount_t *mount = (ramfs_mount_t *)ctx;
    int ret = -1;

    if (length < 0) {
        errno = EINVAL;
        return -1;
    }

    ramfs_lock(mount);
    ramfs_node_t *node = ramfs_find_node(mount, path);
    if (node != NULL) {
        ret = ramfs_resize_node(mount, node, (size_t)length);
    }
    ramfs_unlock(mount);
    return ret;
}

static int ramfs_vfs_ftruncate(void *ctx, int fd, off_t length)
{
    ramfs_mount_t *mount = (ramfs_mount_t *)ctx;
    int ret = -1;

    if (length < 0) {
        errno = EINVAL;
        return -1;
    }

    ramfs_lock(mount);
    ramfs_file_t *file = ramfs_get_file(mount, fd);
    if (file != NULL) {
        ret = ramfs_resize_node(mount, file->node, (size_t)length);
        if (ret == 0 && file->offset > (size_t)length) {
            file->offset = (size_t)length;
        }
    }
    ramfs_unlock(mount);
    return ret;
}

static int ramfs_vfs_fsync(void *ctx, int fd)
{
    ramfs_mount_t *mount = (ramfs_mount_t *)ctx;
    int ret = -1;

    ramfs_lock(mount);
    ret = ramfs_get_file(mount, fd) == NULL ? -1 : 0;
    ramfs_unlock(mount);
    return ret;
}

static const esp_vfs_t ramfs_vfs = {
    .flags = ESP_VFS_FLAG_CONTEXT_PTR,
    .write_p = ramfs_vfs_write,
    .lseek_p = ramfs_vfs_lseek,
    .read_p = ramfs_vfs_read,
    .open_p = ramfs_vfs_open,
    .close_p = ramfs_vfs_close,
    .fstat_p = ramfs_vfs_fstat,
    .fsync_p = ramfs_vfs_fsync,
#ifdef CONFIG_VFS_SUPPORT_DIR
    .stat_p = ramfs_vfs_stat,
    .unlink_p = ramfs_vfs_unlink,
    .rename_p = ramfs_vfs_rename,
    .opendir_p = ramfs_vfs_opendir,
    .readdir_p = ramfs_vfs_readdir,
    .readdir_r_p = ramfs_vfs_readdir_r,
    .telldir_p = ramfs_vfs_telldir,
    .seekdir_p = ramfs_vfs_seekdir,
    .closedir_p = ramfs_vfs_closedir,
    .mkdir_p = ramfs_vfs_mkdir,
    .rmdir_p = ramfs_vfs_rmdir,
    .access_p = ramfs_vfs_access,
    .truncate_p = ramfs_vfs_truncate,
    .ftruncate_p = ramfs_vfs_ftruncate,
#endif
};

static ramfs_mount_t *ramfs_find_mount(const char *mount_point)
{
    if (mount_point == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < SOLAR_OS_RAMFS_MAX_MOUNTS; i++) {
        if (mounts[i].active && strcmp(mounts[i].mount_point, mount_point) == 0) {
            return &mounts[i];
        }
    }
    return NULL;
}

static ramfs_mount_t *ramfs_find_path_mount(const char *path)
{
    ramfs_mount_t *best = NULL;
    size_t best_len = 0;

    for (size_t i = 0; i < SOLAR_OS_RAMFS_MAX_MOUNTS; i++) {
        if (!mounts[i].active) {
            continue;
        }
        const size_t len = strlen(mounts[i].mount_point);
        if (len > best_len && ramfs_path_is_on_mount(path, mounts[i].mount_point)) {
            best = &mounts[i];
            best_len = len;
        }
    }
    return best;
}

static bool ramfs_mount_overlaps(const char *mount_point)
{
    for (size_t i = 0; i < SOLAR_OS_RAMFS_MAX_MOUNTS; i++) {
        if (!mounts[i].active) {
            continue;
        }
        if (ramfs_path_is_on_mount(mount_point, mounts[i].mount_point) ||
            ramfs_path_is_on_mount(mounts[i].mount_point, mount_point)) {
            return true;
        }
    }
    return false;
}

static esp_err_t ramfs_validate_mount_point(const char *mount_point)
{
    if (mount_point == NULL || mount_point[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(mount_point, "/") == 0) {
        return ESP_OK;
    }
    const size_t len = strlen(mount_point);
    if (len == 0 || len >= SOLAR_OS_RAMFS_MOUNT_POINT_MAX || len > ESP_VFS_PATH_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (mount_point[len - 1] == '/') {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 1; i < len; i++) {
        if (mount_point[i] == '/' && mount_point[i - 1] == '/') {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

esp_err_t solar_os_ramfs_mount(const char *mount_point, size_t quota_bytes)
{
#if !SOLAR_OS_BOARD_HAS_PSRAM
    (void)mount_point;
    (void)quota_bytes;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (quota_bytes < RAMFS_MIN_QUOTA_BYTES || quota_bytes <= sizeof(ramfs_block_t)) {
        return ESP_ERR_INVALID_SIZE;
    }
    solar_os_memory_status_t memory_status;
    solar_os_memory_get_status(&memory_status);
    if (memory_status.external.total == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t ret = ramfs_validate_mount_point(mount_point);
    if (ret != ESP_OK) {
        return ret;
    }
    if (ramfs_mount_overlaps(mount_point)) {
        return ESP_ERR_INVALID_STATE;
    }

    ramfs_mount_t *mount = NULL;
    for (size_t i = 0; i < SOLAR_OS_RAMFS_MAX_MOUNTS; i++) {
        if (!mounts[i].active) {
            mount = &mounts[i];
            break;
        }
    }
    if (mount == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(mount, 0, sizeof(*mount));
    mount->arena = solar_os_memory_alloc(quota_bytes,
                                         SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
                                         "ramfs.arena");
    if (mount->arena == NULL) {
        memset(mount, 0, sizeof(*mount));
        return ESP_ERR_NO_MEM;
    }
    mount->arena_bytes = quota_bytes;
    mount->quota_bytes = quota_bytes - sizeof(ramfs_block_t);
    mount->blocks = (ramfs_block_t *)mount->arena;
    mount->blocks->size = mount->quota_bytes;
    mount->blocks->free = true;
    mount->blocks->prev = NULL;
    mount->blocks->next = NULL;
    mount->root.name = "";
    mount->root.is_dir = true;
    mount->root.mtime = time(NULL);
    mount->dir_count = 1;
    mount->lock = xSemaphoreCreateMutex();
    if (mount->lock == NULL) {
        solar_os_memory_free(mount->arena);
        memset(mount, 0, sizeof(*mount));
        return ESP_ERR_NO_MEM;
    }
    strlcpy(mount->mount_point, mount_point, sizeof(mount->mount_point));

    ret = esp_vfs_register(ramfs_vfs_base_path(mount->mount_point), &ramfs_vfs, mount);
    if (ret != ESP_OK) {
        vSemaphoreDelete(mount->lock);
        solar_os_memory_free(mount->arena);
        memset(mount, 0, sizeof(*mount));
        return ret;
    }
    mount->active = true;
    return ESP_OK;
#endif
}

esp_err_t solar_os_ramfs_unmount(const char *mount_point)
{
#if !SOLAR_OS_BOARD_HAS_PSRAM
    (void)mount_point;
    return ESP_ERR_NOT_SUPPORTED;
#else
    ramfs_mount_t *mount = ramfs_find_mount(mount_point);
    if (mount == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    ramfs_lock(mount);
    for (size_t i = 0; i < RAMFS_MAX_OPEN_FILES; i++) {
        if (mount->files[i].active) {
            ramfs_unlock(mount);
            return ESP_ERR_INVALID_STATE;
        }
    }
    ramfs_unlock(mount);

    esp_err_t ret = esp_vfs_unregister(ramfs_vfs_base_path(mount->mount_point));
    if (ret != ESP_OK) {
        return ret;
    }

    ramfs_lock(mount);
    ramfs_free_tree(mount, &mount->root);
    ramfs_unlock(mount);
    vSemaphoreDelete(mount->lock);
    solar_os_memory_free(mount->arena);
    memset(mount, 0, sizeof(*mount));
    return ESP_OK;
#endif
}

size_t solar_os_ramfs_mount_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < SOLAR_OS_RAMFS_MAX_MOUNTS; i++) {
        if (mounts[i].active) {
            count++;
        }
    }
    return count;
}

bool solar_os_ramfs_get_info(size_t index, solar_os_ramfs_info_t *info)
{
    if (info == NULL) {
        return false;
    }

    size_t seen = 0;
    for (size_t i = 0; i < SOLAR_OS_RAMFS_MAX_MOUNTS; i++) {
        ramfs_mount_t *mount = &mounts[i];
        if (!mount->active) {
            continue;
        }
        if (seen++ != index) {
            continue;
        }

        memset(info, 0, sizeof(*info));
        ramfs_lock(mount);
        strlcpy(info->mount_point, mount->mount_point, sizeof(info->mount_point));
        ramfs_get_space_locked(mount, &info->total_bytes, &info->used_bytes, &info->free_bytes);
        info->file_count = mount->file_count;
        info->dir_count = mount->dir_count;
        for (size_t fd = 0; fd < RAMFS_MAX_OPEN_FILES; fd++) {
            if (mount->files[fd].active) {
                info->open_count++;
            }
        }
        ramfs_unlock(mount);
        return true;
    }
    return false;
}

bool solar_os_ramfs_path_has_mount_prefix(const char *path)
{
    return ramfs_find_path_mount(path) != NULL;
}

esp_err_t solar_os_ramfs_path_mount_point(const char *path,
                                          char *mount_point,
                                          size_t mount_point_len)
{
    if (path == NULL || mount_point == NULL || mount_point_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ramfs_mount_t *mount = ramfs_find_path_mount(path);
    if (mount == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (strlcpy(mount_point, mount->mount_point, mount_point_len) >= mount_point_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t solar_os_ramfs_get_usage_for_path(const char *path,
                                            uint64_t *total_bytes,
                                            uint64_t *used_bytes,
                                            uint64_t *free_bytes)
{
    if (path == NULL || total_bytes == NULL || used_bytes == NULL || free_bytes == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ramfs_mount_t *mount = ramfs_find_path_mount(path);
    if (mount == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    ramfs_lock(mount);
    ramfs_get_space_locked(mount, total_bytes, used_bytes, free_bytes);
    ramfs_unlock(mount);
    return ESP_OK;
}
