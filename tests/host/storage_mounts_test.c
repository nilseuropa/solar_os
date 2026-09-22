#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "flash_storage.h"
#include "solar_os_board_storage.h"
#include "solar_os_ramfs.h"
#include "solar_os_storage.h"

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t len = strlen(src);
    if (size > 0) {
        const size_t copy = len >= size ? size - 1 : len;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

size_t solar_os_board_storage_block_count(void)
{
    return 2;
}

bool solar_os_board_storage_available(void)
{
    return true;
}

bool solar_os_board_storage_get_block(size_t index, solar_os_board_storage_block_t *block)
{
    if (block == NULL || index >= solar_os_board_storage_block_count()) {
        return false;
    }

    memset(block, 0, sizeof(*block));
    if (index == 0) {
        strlcpy(block->name, "sd0", sizeof(block->name));
        block->type = SOLAR_OS_BOARD_STORAGE_BLOCK_DISK;
        return true;
    }

    strlcpy(block->name, "sd0p1", sizeof(block->name));
    strlcpy(block->mount_point, "/sdcard", sizeof(block->mount_point));
    block->type = SOLAR_OS_BOARD_STORAGE_BLOCK_PARTITION;
    block->mounted = true;
    block->mountable = true;
    block->logical_volume = 0;
    return true;
}

bool solar_os_board_storage_is_mounted(void)
{
    return true;
}

const char *solar_os_board_storage_mount_point(void)
{
    return "/sdcard";
}

bool flash_storage_is_mounted(void)
{
    return true;
}

const char *flash_storage_mount_point(void)
{
    return "/flash";
}

uint8_t flash_storage_logical_volume(void)
{
    return 1;
}

uint64_t flash_storage_size_bytes(void)
{
    return 600U * 1024U;
}

size_t solar_os_ramfs_mount_count(void)
{
    return 1;
}

bool solar_os_ramfs_get_info(size_t index, solar_os_ramfs_info_t *info)
{
    if (index != 0 || info == NULL) {
        return false;
    }
    memset(info, 0, sizeof(*info));
    strlcpy(info->mount_point, "/ram", sizeof(info->mount_point));
    return true;
}

static void assert_mount(size_t index,
                         const char *name,
                         const char *mount_point,
                         solar_os_storage_mount_type_t type)
{
    solar_os_storage_mount_info_t mount;
    assert(solar_os_storage_get_mount(index, &mount));
    assert(strcmp(mount.name, name) == 0);
    assert(strcmp(mount.mount_point, mount_point) == 0);
    assert(mount.type == type);
}

typedef struct {
    uint64_t last_done;
    uint64_t total;
    size_t calls;
} copy_progress_t;

static void copy_progress(uint64_t bytes_done, uint64_t bytes_total, void *user)
{
    copy_progress_t *progress = (copy_progress_t *)user;
    assert(progress != NULL);
    assert(bytes_done >= progress->last_done);
    assert(bytes_done <= bytes_total);
    progress->last_done = bytes_done;
    progress->total = bytes_total;
    progress->calls++;
}

static void assert_copy_progress(void)
{
    char source[] = "/tmp/solaros-storage-source-XXXXXX";
    char dest[] = "/tmp/solaros-storage-dest-XXXXXX";
    const int source_fd = mkstemp(source);
    const int dest_fd = mkstemp(dest);
    assert(source_fd >= 0);
    assert(dest_fd >= 0);

    uint8_t payload[8193];
    for (size_t i = 0U; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i & 0xffU);
    }
    assert(write(source_fd, payload, sizeof(payload)) == (ssize_t)sizeof(payload));
    assert(close(source_fd) == 0);
    assert(close(dest_fd) == 0);

    copy_progress_t progress = {0};
    assert(solar_os_storage_copy_file_progress(source,
                                               dest,
                                               copy_progress,
                                               &progress) == ESP_OK);
    assert(progress.calls >= 2U);
    assert(progress.last_done == sizeof(payload));
    assert(progress.total == sizeof(payload));

    FILE *copied = fopen(dest, "rb");
    assert(copied != NULL);
    uint8_t actual[sizeof(payload)];
    assert(fread(actual, 1U, sizeof(actual), copied) == sizeof(actual));
    assert(fclose(copied) == 0);
    assert(memcmp(actual, payload, sizeof(payload)) == 0);
    assert(remove(source) == 0);
    assert(remove(dest) == 0);
}

static bool cancel_copy_after_progress(void *user)
{
    const copy_progress_t *progress = (const copy_progress_t *)user;
    return progress != NULL && progress->calls >= 2U;
}

static void assert_copy_cancel(void)
{
    char source[] = "/tmp/solaros-storage-cancel-source-XXXXXX";
    char dest[] = "/tmp/solaros-storage-cancel-dest-XXXXXX";
    const int source_fd = mkstemp(source);
    const int dest_fd = mkstemp(dest);
    assert(source_fd >= 0);
    assert(dest_fd >= 0);

    uint8_t payload[8193];
    memset(payload, 0x5a, sizeof(payload));
    assert(write(source_fd, payload, sizeof(payload)) == (ssize_t)sizeof(payload));
    assert(close(source_fd) == 0);
    assert(close(dest_fd) == 0);

    copy_progress_t progress = {0};
    errno = 0;
    assert(solar_os_storage_copy_file_progress_cancel(source,
                                                      dest,
                                                      copy_progress,
                                                      cancel_copy_after_progress,
                                                      &progress) ==
           ESP_ERR_INVALID_STATE);
    assert(errno == ECANCELED);
    assert(progress.calls >= 2U);
    assert(access(dest, F_OK) != 0);
    assert(remove(source) == 0);
}

static void write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    assert(fputs(text, file) >= 0);
    assert(solar_os_storage_sync_file(file) == ESP_OK);
    assert(fclose(file) == 0);
}

static void assert_file_text(const char *path, const char *expected)
{
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    char actual[32];
    assert(fgets(actual, sizeof(actual), file) != NULL);
    assert(fclose(file) == 0);
    assert(strcmp(actual, expected) == 0);
}

static void assert_replace_file(void)
{
    char active[] = "/tmp/solaros-storage-active-XXXXXX";
    const int active_fd = mkstemp(active);
    assert(active_fd >= 0);
    assert(close(active_fd) == 0);

    char staged[SOLAR_OS_STORAGE_PATH_MAX];
    char backup[SOLAR_OS_STORAGE_PATH_MAX];
    assert(solar_os_storage_sibling_path(
        active, ".tmp", staged, sizeof(staged)) == ESP_OK);
    assert(solar_os_storage_sibling_path(
        active, ".bak", backup, sizeof(backup)) == ESP_OK);

    write_text(active, "old");
    write_text(staged, "new");
    write_text(backup, "stale");
    assert(solar_os_storage_replace_file(staged, active, backup) == ESP_OK);
    assert_file_text(active, "new");
    assert(access(staged, F_OK) != 0);
    assert(access(backup, F_OK) != 0);

    write_text(staged, "first");
    assert(remove(active) == 0);
    assert(solar_os_storage_replace_file(staged, active, backup) == ESP_OK);
    assert_file_text(active, "first");
    assert(remove(active) == 0);

    assert(solar_os_storage_replace_file(staged, active, backup) ==
           ESP_ERR_NOT_FOUND);
    assert(solar_os_storage_sibling_path(
        active, ".too-long", staged, 4U) == ESP_ERR_INVALID_SIZE);
}

static void assert_storage_discovery(void)
{
    char root[] = "/tmp/solaros-storage-scan-XXXXXX";
    assert(mkdtemp(root) != NULL);

    char alpha[SOLAR_OS_STORAGE_PATH_MAX];
    char beta[SOLAR_OS_STORAGE_PATH_MAX];
    char gamma[SOLAR_OS_STORAGE_PATH_MAX];
    char nested[SOLAR_OS_STORAGE_PATH_MAX];
    char nested_parent[SOLAR_OS_STORAGE_PATH_MAX];
    assert(snprintf(alpha, sizeof(alpha), "%s/alpha.txt", root) > 0);
    assert(snprintf(beta, sizeof(beta), "%s/beta", root) > 0);
    assert(snprintf(gamma, sizeof(gamma), "%s/gamma.txt", root) > 0);
    assert(snprintf(nested, sizeof(nested), "%s/one/two", root) > 0);
    assert(snprintf(nested_parent, sizeof(nested_parent), "%s/one", root) > 0);

    write_text(alpha, "abc");
    write_text(gamma, "hello");
    assert(solar_os_storage_mkdir(beta) == ESP_OK);
    assert(solar_os_storage_makedirs(nested, true) == ESP_OK);
    assert(solar_os_storage_makedirs(nested, true) == ESP_OK);
    errno = 0;
    assert(solar_os_storage_makedirs(nested, false) == ESP_ERR_INVALID_STATE);
    assert(errno == EEXIST);

    solar_os_storage_metadata_t metadata;
    assert(solar_os_storage_stat(alpha, &metadata) == ESP_OK);
    assert(metadata.type == SOLAR_OS_STORAGE_ENTRY_FILE);
    assert(metadata.size_bytes == 3U);
    assert(solar_os_storage_stat(beta, &metadata) == ESP_OK);
    assert(metadata.type == SOLAR_OS_STORAGE_ENTRY_DIRECTORY);

    bool exists = false;
    assert(solar_os_storage_exists(alpha, &exists) == ESP_OK);
    assert(exists);
    char missing[SOLAR_OS_STORAGE_PATH_MAX];
    assert(snprintf(missing, sizeof(missing), "%s/missing", root) > 0);
    assert(solar_os_storage_exists(missing, &exists) == ESP_OK);
    assert(!exists);

    bool saw_alpha = false;
    bool saw_beta = false;
    bool saw_gamma = false;
    bool saw_one = false;
    size_t cursor = 0U;
    size_t total = 0U;
    bool has_more = false;
    do {
        solar_os_storage_entry_t entries[2];
        size_t count = 0U;
        size_t next_cursor = cursor;
        assert(solar_os_storage_scandir(root,
                                        cursor,
                                        2U,
                                        entries,
                                        &count,
                                        &next_cursor,
                                        &has_more) == ESP_OK);
        assert(count <= 2U);
        for (size_t i = 0U; i < count; i++) {
            if (strcmp(entries[i].name, "alpha.txt") == 0) {
                assert(!saw_alpha);
                saw_alpha = true;
                assert(entries[i].metadata.type == SOLAR_OS_STORAGE_ENTRY_FILE);
            } else if (strcmp(entries[i].name, "beta") == 0) {
                assert(!saw_beta);
                saw_beta = true;
                assert(entries[i].metadata.type == SOLAR_OS_STORAGE_ENTRY_DIRECTORY);
            } else if (strcmp(entries[i].name, "gamma.txt") == 0) {
                assert(!saw_gamma);
                saw_gamma = true;
                assert(entries[i].metadata.type == SOLAR_OS_STORAGE_ENTRY_FILE);
            } else if (strcmp(entries[i].name, "one") == 0) {
                assert(!saw_one);
                saw_one = true;
                assert(entries[i].metadata.type == SOLAR_OS_STORAGE_ENTRY_DIRECTORY);
            } else {
                assert(false);
            }
        }
        total += count;
        if (has_more) {
            assert(count == 2U);
            assert(next_cursor == cursor + count);
            cursor = next_cursor;
        }
    } while (has_more);
    assert(total == 4U);
    assert(saw_alpha && saw_beta && saw_gamma && saw_one);

    assert(remove(alpha) == 0);
    assert(remove(gamma) == 0);
    assert(rmdir(nested) == 0);
    assert(rmdir(nested_parent) == 0);
    assert(rmdir(beta) == 0);
    assert(rmdir(root) == 0);
}

int main(void)
{
    assert(solar_os_storage_block_count() == 3);
    assert(solar_os_storage_mount_count() == 3);
    assert_mount(0, "sd0p1", "/sdcard", SOLAR_OS_STORAGE_MOUNT_SD);
    assert_mount(1, "flash", "/flash", SOLAR_OS_STORAGE_MOUNT_FLASH);
    assert_mount(2, "ramfs", "/ram", SOLAR_OS_STORAGE_MOUNT_RAMFS);

    solar_os_storage_mount_info_t mount;
    assert(!solar_os_storage_get_mount(3, &mount));

    char path[SOLAR_OS_STORAGE_PATH_MAX];
    assert(solar_os_storage_default_path(".player", path, sizeof(path)) == ESP_OK);
    assert(strcmp(path, "/sdcard/.player") == 0);

    assert_copy_progress();
    assert_copy_cancel();
    assert_replace_file();
    assert_storage_discovery();

    puts("storage mount tests: ok");
    return 0;
}
