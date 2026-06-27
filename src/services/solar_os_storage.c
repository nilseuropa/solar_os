#include "solar_os_storage.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ff.h"
#include "solar_os_board_caps.h"
#if SOLAR_OS_BOARD_HAS_SD
#include "solar_os_board_storage.h"
#endif

#define SOLAR_OS_STORAGE_COPY_BUFFER_SIZE 512
#define SOLAR_OS_STORAGE_DEFAULT_MOUNT_POINT "/sdcard"

static esp_err_t get_usage_for_fatfs_path(const char *fatfs_path, solar_os_storage_usage_t *usage)
{
    if (usage == NULL || fatfs_path == NULL || fatfs_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    DWORD free_clusters = 0;
    FATFS *fs = NULL;
    const FRESULT result = f_getfree(fatfs_path, &free_clusters, &fs);
    if (result != FR_OK || fs == NULL) {
        return ESP_FAIL;
    }

#if FF_MAX_SS != FF_MIN_SS
    const uint32_t sector_size = fs->ssize;
#else
    const uint32_t sector_size = FF_MAX_SS;
#endif
    const uint64_t cluster_size = (uint64_t)fs->csize * sector_size;
    const uint64_t total_clusters = fs->n_fatent > 2 ? (uint64_t)fs->n_fatent - 2ULL : 0ULL;

    usage->total_bytes = total_clusters * cluster_size;
    usage->free_bytes = (uint64_t)free_clusters * cluster_size;
    usage->used_bytes =
        usage->total_bytes >= usage->free_bytes ? usage->total_bytes - usage->free_bytes : 0ULL;
    return ESP_OK;
}

static bool path_is_on_mount(const char *path, const char *mount_point)
{
    if (path == NULL || mount_point == NULL || mount_point[0] == '\0') {
        return false;
    }

    const size_t len = strlen(mount_point);
    return strncmp(path, mount_point, len) == 0 &&
        (path[len] == '\0' || path[len] == '/');
}

esp_err_t solar_os_storage_init(void)
{
    return solar_os_storage_mount();
}

esp_err_t solar_os_storage_mount(void)
{
#if SOLAR_OS_BOARD_HAS_SD
    return solar_os_board_storage_mount();
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t solar_os_storage_mount_volume(const char *name, const char *mount_point)
{
#if SOLAR_OS_BOARD_HAS_SD
    return solar_os_board_storage_mount_volume(name, mount_point);
#else
    (void)name;
    (void)mount_point;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t solar_os_storage_unmount(void)
{
#if SOLAR_OS_BOARD_HAS_SD
    return solar_os_board_storage_unmount();
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t solar_os_storage_unmount_volume(const char *target)
{
#if SOLAR_OS_BOARD_HAS_SD
    return solar_os_board_storage_unmount_volume(target);
#else
    (void)target;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool solar_os_storage_is_mounted(void)
{
#if SOLAR_OS_BOARD_HAS_SD
    return solar_os_board_storage_is_mounted();
#else
    return false;
#endif
}

void solar_os_storage_get_status(char *buffer, size_t len)
{
#if SOLAR_OS_BOARD_HAS_SD
    solar_os_board_storage_get_status(buffer, len);
#else
    if (buffer != NULL && len > 0) {
        strlcpy(buffer, "unavailable", len);
    }
#endif
}

const char *solar_os_storage_mount_point(void)
{
#if SOLAR_OS_BOARD_HAS_SD
    return solar_os_board_storage_mount_point();
#else
    return SOLAR_OS_STORAGE_DEFAULT_MOUNT_POINT;
#endif
}

esp_err_t solar_os_storage_get_usage(solar_os_storage_usage_t *usage)
{
    return solar_os_storage_get_usage_for_path(solar_os_storage_mount_point(), usage);
}

esp_err_t solar_os_storage_get_usage_for_path(const char *path, solar_os_storage_usage_t *usage)
{
    if (usage == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (path == NULL || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
#if !SOLAR_OS_BOARD_HAS_SD
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!solar_os_storage_is_mounted() && strcmp(path, solar_os_storage_mount_point()) == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    solar_os_storage_block_t match;
    bool found = false;
    size_t best_len = 0;
    const size_t count = solar_os_storage_block_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_storage_block_t block;
        if (!solar_os_storage_get_block(i, &block) || !block.mounted) {
            continue;
        }

        const size_t mount_len = strlen(block.mount_point);
        if (mount_len > best_len && path_is_on_mount(path, block.mount_point)) {
            match = block;
            found = true;
            best_len = mount_len;
        }
    }

    return found ? solar_os_storage_get_usage_for_block(&match, usage) : ESP_ERR_NOT_FOUND;
#endif
}

esp_err_t solar_os_storage_get_usage_for_block(const solar_os_storage_block_t *block,
                                               solar_os_storage_usage_t *usage)
{
    if (block == NULL || usage == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!block->mounted || block->logical_volume == SOLAR_OS_STORAGE_LOGICAL_VOLUME_INVALID) {
        return ESP_ERR_INVALID_STATE;
    }

#if !SOLAR_OS_BOARD_HAS_SD
    return ESP_ERR_NOT_SUPPORTED;
#else
    char drive[3] = {(char)('0' + block->logical_volume), ':', '\0'};
    return get_usage_for_fatfs_path(drive, usage);
#endif
}

esp_err_t solar_os_storage_rescan(void)
{
#if SOLAR_OS_BOARD_HAS_SD
    return solar_os_board_storage_rescan();
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

size_t solar_os_storage_block_count(void)
{
#if SOLAR_OS_BOARD_HAS_SD
    return solar_os_board_storage_block_count();
#else
    return 0;
#endif
}

bool solar_os_storage_get_block(size_t index, solar_os_storage_block_t *block)
{
    if (block == NULL) {
        return false;
    }

#if !SOLAR_OS_BOARD_HAS_SD
    (void)index;
    return false;
#else
    solar_os_board_storage_block_t board_block;
    if (!solar_os_board_storage_get_block(index, &board_block)) {
        return false;
    }

    memset(block, 0, sizeof(*block));
    strlcpy(block->name, board_block.name, sizeof(block->name));
    block->type = board_block.type == SOLAR_OS_BOARD_STORAGE_BLOCK_PARTITION ?
        SOLAR_OS_STORAGE_BLOCK_PARTITION :
        SOLAR_OS_STORAGE_BLOCK_DISK;
    block->partition_number = board_block.partition_number;
    block->mbr_type = board_block.mbr_type;
    block->bootable = board_block.bootable;
    block->mountable = board_block.mountable;
    block->mounted = board_block.mounted;
    block->whole_disk_filesystem = board_block.whole_disk_filesystem;
    block->logical_volume = board_block.logical_volume;
    block->start_sector = board_block.start_sector;
    block->sector_count = board_block.sector_count;
    block->sector_size = board_block.sector_size;
    block->size_bytes = board_block.size_bytes;
    strlcpy(block->fs, board_block.fs, sizeof(block->fs));
    strlcpy(block->type_name, board_block.type_name, sizeof(block->type_name));
    strlcpy(block->mount_point, board_block.mount_point, sizeof(block->mount_point));
    return true;
#endif
}

bool solar_os_storage_path_has_mount_prefix(const char *path)
{
    char mount_point[SOLAR_OS_STORAGE_MOUNT_POINT_MAX];
    return solar_os_storage_path_mount_point(path, mount_point, sizeof(mount_point)) == ESP_OK;
}

esp_err_t solar_os_storage_path_mount_point(const char *path,
                                            char *mount_point,
                                            size_t mount_point_len)
{
    if (path == NULL || path[0] == '\0' || mount_point == NULL || mount_point_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
#if !SOLAR_OS_BOARD_HAS_SD
    return ESP_ERR_NOT_SUPPORTED;
#else

    char best_mount[SOLAR_OS_STORAGE_MOUNT_POINT_MAX] = {0};
    size_t best_len = 0;
    const char *default_mount = solar_os_storage_mount_point();
    if (path_is_on_mount(path, default_mount)) {
        best_len = strlen(default_mount);
        if (strlcpy(best_mount, default_mount, sizeof(best_mount)) >= sizeof(best_mount)) {
            return ESP_ERR_INVALID_SIZE;
        }
    }

    const size_t count = solar_os_storage_block_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_storage_block_t block;
        if (!solar_os_storage_get_block(i, &block) ||
            !block.mounted ||
            block.mount_point[0] == '\0') {
            continue;
        }

        const size_t mount_len = strlen(block.mount_point);
        if (mount_len > best_len && path_is_on_mount(path, block.mount_point)) {
            best_len = mount_len;
            strlcpy(best_mount, block.mount_point, sizeof(best_mount));
        }
    }

    if (best_mount[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    if (strlcpy(mount_point, best_mount, mount_point_len) >= mount_point_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
#endif
}

static esp_err_t storage_append_path_segment(char *out,
                                             size_t out_len,
                                             const char *segment,
                                             size_t segment_len)
{
    const size_t out_used = strlen(out);
    const bool needs_slash = !(out_used == 1 && out[0] == '/');
    const size_t slash_len = needs_slash ? 1 : 0;

    if (out_used + slash_len + segment_len >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (needs_slash) {
        out[out_used] = '/';
    }
    memcpy(&out[out_used + slash_len], segment, segment_len);
    out[out_used + slash_len + segment_len] = '\0';
    return ESP_OK;
}

static void storage_pop_path_segment(char *out, size_t root_len)
{
    const size_t len = strlen(out);
    if (len <= root_len) {
        out[root_len] = '\0';
        return;
    }

    char *slash = strrchr(out, '/');
    if (slash == NULL || (size_t)(slash - out) <= root_len) {
        out[root_len] = '\0';
        return;
    }

    *slash = '\0';
}

esp_err_t solar_os_storage_normalize_path(const char *path, char *out, size_t out_len)
{
    if (path == NULL || path[0] == '\0' || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char root[SOLAR_OS_STORAGE_MOUNT_POINT_MAX];
    esp_err_t ret = solar_os_storage_path_mount_point(path, root, sizeof(root));
    if (ret != ESP_OK) {
        return ret;
    }

    const size_t root_len = strlen(root);
    if (root_len >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    strlcpy(out, root, out_len);

    const char *cursor = path + root_len;
    while (*cursor == '/') {
        cursor++;
    }

    while (*cursor != '\0') {
        const char *segment = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            cursor++;
        }
        const size_t segment_len = (size_t)(cursor - segment);

        while (*cursor == '/') {
            cursor++;
        }

        if (segment_len == 0 ||
            (segment_len == 1 && segment[0] == '.')) {
            continue;
        }

        if (segment_len == 2 && segment[0] == '.' && segment[1] == '.') {
            storage_pop_path_segment(out, root_len);
            continue;
        }

        ret = storage_append_path_segment(out, out_len, segment, segment_len);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ESP_OK;
}

esp_err_t solar_os_storage_resolve_path_at(const char *cwd,
                                           const char *arg,
                                           char *path,
                                           size_t path_len)
{
    if (path == NULL || path_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *base = cwd;
    if (!solar_os_storage_path_has_mount_prefix(base)) {
        base = solar_os_storage_mount_point();
    }

    char raw[SOLAR_OS_STORAGE_PATH_MAX];
    int written = 0;
    if (arg == NULL || arg[0] == '\0') {
        if (strlcpy(raw, base, sizeof(raw)) >= sizeof(raw)) {
            return ESP_ERR_INVALID_SIZE;
        }
    } else if (arg[0] == '/') {
        if (solar_os_storage_path_has_mount_prefix(arg)) {
            if (strlcpy(raw, arg, sizeof(raw)) >= sizeof(raw)) {
                return ESP_ERR_INVALID_SIZE;
            }
        } else {
            written = snprintf(raw, sizeof(raw), "%s%s", solar_os_storage_mount_point(), arg);
            if (written < 0 || (size_t)written >= sizeof(raw)) {
                return ESP_ERR_INVALID_SIZE;
            }
        }
    } else {
        written = snprintf(raw, sizeof(raw), "%s/%s", base, arg);
        if (written < 0 || (size_t)written >= sizeof(raw)) {
            return ESP_ERR_INVALID_SIZE;
        }
    }

    return solar_os_storage_normalize_path(raw, path, path_len);
}

esp_err_t solar_os_storage_resolve_path(const char *arg, char *path, size_t path_len)
{
    return solar_os_storage_resolve_path_at(NULL, arg, path, path_len);
}

esp_err_t solar_os_storage_mkdir(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }

    return mkdir(path, 0777) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t solar_os_storage_rmdir(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }

    return rmdir(path) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t solar_os_storage_remove(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }

    return remove(path) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t solar_os_storage_rename(const char *old_path, const char *new_path)
{
    if (old_path == NULL || old_path[0] == '\0' || new_path == NULL || new_path[0] == '\0') {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }

    return rename(old_path, new_path) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t solar_os_storage_copy_file(const char *source_path, const char *dest_path)
{
    if (source_path == NULL || source_path[0] == '\0' || dest_path == NULL || dest_path[0] == '\0') {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(source_path, dest_path) == 0) {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }

    FILE *source = fopen(source_path, "rb");
    if (source == NULL) {
        return ESP_FAIL;
    }

    FILE *dest = fopen(dest_path, "wb");
    if (dest == NULL) {
        const int open_errno = errno;
        fclose(source);
        errno = open_errno;
        return ESP_FAIL;
    }

    uint8_t buffer[SOLAR_OS_STORAGE_COPY_BUFFER_SIZE];
    esp_err_t ret = ESP_OK;

    while (true) {
        const size_t bytes_read = fread(buffer, 1, sizeof(buffer), source);
        if (bytes_read > 0 && fwrite(buffer, 1, bytes_read, dest) != bytes_read) {
            ret = ESP_FAIL;
            break;
        }

        if (bytes_read < sizeof(buffer)) {
            if (ferror(source)) {
                ret = ESP_FAIL;
            }
            break;
        }
    }

    const int copy_errno = errno;
    if (fclose(dest) != 0 && ret == ESP_OK) {
        ret = ESP_FAIL;
    }
    const int close_errno = errno;
    fclose(source);

    if (ret != ESP_OK) {
        errno = close_errno != 0 ? close_errno : (copy_errno != 0 ? copy_errno : EIO);
    }
    return ret;
}
