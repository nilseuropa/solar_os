#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "solar_os_storage.h"
#include "solar_os_writer_buffer.h"
#include "solar_os_writer_files.h"

static struct {
    solar_os_writer_buffer_t buffer;
    bool dirty, recovery_written;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    char metadata_path[SOLAR_OS_STORAGE_PATH_MAX];
    char recovery_path[SOLAR_OS_STORAGE_PATH_MAX];
    size_t cursor;
    int zoom;
} writer;
static const char *fail_publish;
static unsigned rename_conflicts;

/* Model FatFs: rename must not overwrite an existing destination. */
int writer_test_rename(const char *src, const char *dst)
{
    struct stat st;
    if (stat(dst, &st) == 0) {
        rename_conflicts++;
        errno = EEXIST;
        return -1;
    }
    if (fail_publish != NULL && strcmp(dst, fail_publish) == 0 &&
        strstr(src, ".tmp") != NULL) {
        fail_publish = NULL;
        errno = EIO;
        return -1;
    }
    return renameat(AT_FDCWD, src, AT_FDCWD, dst);
}

static size_t writer_scroll_anchor(void) { return writer.cursor; }
static void solar_os_memory_free(void *ptr) { free(ptr); }
#include "writer_recovery.inc"

static void expect_file(const char *path, const char *expected)
{
    char data[512];
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    const size_t len = fread(data, 1, sizeof(data), file);
    assert(!ferror(file));
    assert(fclose(file) == 0);
    assert(len == strlen(expected));
    assert(memcmp(data, expected, len) == 0);
}

static void edit(const char *text, size_t cursor)
{
    assert(solar_os_writer_buffer_set(&writer.buffer, text, strlen(text)) == ESP_OK);
    writer.cursor = cursor;
    writer.dirty = true;
    writer.recovery_written = false;
}

int main(void)
{
    char dir[] = "/tmp/solaros-writer-recovery-XXXXXX";
    assert(mkdtemp(dir) != NULL);
    solar_os_writer_buffer_init(&writer.buffer);
    writer.zoom = 1;
    strcpy(writer.path, "document.md");
    assert(snprintf(writer.metadata_path, sizeof(writer.metadata_path), "%s/state.meta", dir) > 0);

    /* Clean documents and unavailable recovery paths must not write anything. */
    assert(writer_write_recovery() == ESP_OK);
    edit("first snapshot", 2);
    assert(writer_write_recovery() == ESP_OK);
    assert(!writer.recovery_written && access(writer.metadata_path, F_OK) != 0);
    assert(snprintf(writer.recovery_path, sizeof(writer.recovery_path), "%s/state.recovery", dir) > 0);
    writer.dirty = false;
    assert(writer_write_recovery() == ESP_OK);
    assert(access(writer.recovery_path, F_OK) != 0);

    writer.dirty = true;
    assert(writer_write_recovery() == ESP_OK && writer.recovery_written);
    expect_file(writer.recovery_path, "first snapshot");
    expect_file(writer.metadata_path, "2 2 1\ndocument.md\n");

    edit("second snapshot with more bytes", 5);
    writer.zoom = 2;
    assert(writer_write_recovery() == ESP_OK && writer.recovery_written);
    expect_file(writer.recovery_path, "second snapshot with more bytes");
    expect_file(writer.metadata_path, "5 5 2\ndocument.md\n");

    edit("third snapshot", 8);
    fail_publish = writer.recovery_path;
    assert(writer_write_recovery() != ESP_OK && !writer.recovery_written);
    assert(fail_publish == NULL);
    expect_file(writer.recovery_path, "second snapshot with more bytes");
    expect_file(writer.metadata_path, "5 5 2\ndocument.md\n");
    assert(writer_write_recovery() == ESP_OK && writer.recovery_written);
    expect_file(writer.recovery_path, "third snapshot");
    expect_file(writer.metadata_path, "8 8 2\ndocument.md\n");

    writer.cursor = 10;
    fail_publish = writer.metadata_path;
    writer_save_metadata();
    assert(fail_publish == NULL);
    expect_file(writer.metadata_path, "8 8 2\ndocument.md\n");
    writer_save_metadata();
    expect_file(writer.metadata_path, "10 10 2\ndocument.md\n");

    edit("", 0);
    assert(writer_write_recovery() == ESP_OK && writer.recovery_written);
    expect_file(writer.recovery_path, "");
    expect_file(writer.metadata_path, "0 0 2\ndocument.md\n");
    assert(rename_conflicts == 0);

    solar_os_writer_buffer_free(&writer.buffer);
    assert(unlink(writer.recovery_path) == 0);
    assert(unlink(writer.metadata_path) == 0);
    assert(rmdir(dir) == 0); /* No staging files or backups left behind. */
    puts("writer recovery: repeated FatFs snapshots, metadata, rollback and retry passed");
    return 0;
}
