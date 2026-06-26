#include "solar_os_zip.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "miniz.h"
#include "solar_os_storage.h"

#define ZIP_LOCAL_FILE_HEADER_SIGNATURE 0x04034b50UL
#define ZIP_CENTRAL_DIRECTORY_SIGNATURE 0x02014b50UL
#define ZIP_END_OF_CENTRAL_DIRECTORY_SIGNATURE 0x06054b50UL

#define ZIP_METHOD_STORE 0U
#define ZIP_METHOD_DEFLATE 8U
#define ZIP_GP_FLAG_ENCRYPTED 0x0001U

#define ZIP_LOCAL_HEADER_LEN 30U
#define ZIP_CENTRAL_HEADER_LEN 46U
#define ZIP_EOCD_LEN 22U
#define ZIP_EOCD_SEARCH_MAX (UINT16_MAX + ZIP_EOCD_LEN)
#define ZIP_IO_BUFFER_SIZE 4096U
#define ZIP_INFLATE_DICT_SIZE TINFL_LZ_DICT_SIZE
#define ZIP_NAME_MAX SOLAR_OS_STORAGE_PATH_MAX

static void zip_yield(void)
{
    vTaskDelay(pdMS_TO_TICKS(1));
}

typedef struct {
    char name[ZIP_NAME_MAX];
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t local_offset;
    uint16_t method;
    bool directory;
} zip_entry_meta_t;

typedef struct {
    FILE *file;
    const char *archive_path;
    const solar_os_zip_options_t *options;
    uint8_t *in_buffer;
    uint8_t *out_buffer;
    zip_entry_meta_t *entries;
    size_t entry_count;
    size_t entry_capacity;
} zip_writer_t;

typedef struct {
    char name[ZIP_NAME_MAX];
    uint32_t crc32;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t local_offset;
    uint16_t flags;
    uint16_t method;
    bool directory;
} zip_reader_entry_t;

typedef struct {
    FILE *file;
    uint16_t entry_count;
    uint32_t central_offset;
    uint32_t central_size;
} zip_reader_t;

static uint16_t zip_read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t zip_read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static bool zip_write_all(FILE *file, const void *data, size_t len)
{
    return len == 0 || fwrite(data, 1, len, file) == len;
}

static bool zip_write_u16(FILE *file, uint16_t value)
{
    const uint8_t data[2] = {
        (uint8_t)(value & 0xffU),
        (uint8_t)((value >> 8) & 0xffU),
    };
    return zip_write_all(file, data, sizeof(data));
}

static bool zip_write_u32(FILE *file, uint32_t value)
{
    const uint8_t data[4] = {
        (uint8_t)(value & 0xffU),
        (uint8_t)((value >> 8) & 0xffU),
        (uint8_t)((value >> 16) & 0xffU),
        (uint8_t)((value >> 24) & 0xffU),
    };
    return zip_write_all(file, data, sizeof(data));
}

static bool zip_tell_u32(FILE *file, uint32_t *position)
{
    const long offset = ftell(file);
    if (offset < 0 || (unsigned long)offset > UINT32_MAX) {
        return false;
    }

    *position = (uint32_t)offset;
    return true;
}

static void *zip_malloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void *zip_calloc(size_t count, size_t size)
{
    void *ptr = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_calloc(count, size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static const char *zip_path_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static bool zip_join_path(char *out, size_t out_len, const char *dir, const char *name)
{
    if (out == NULL || out_len == 0 || dir == NULL || name == NULL) {
        return false;
    }

    const size_t dir_len = strlen(dir);
    const int written = (dir_len > 0 && dir[dir_len - 1] == '/') ?
        snprintf(out, out_len, "%s%s", dir, name) :
        snprintf(out, out_len, "%s/%s", dir, name);
    return written >= 0 && (size_t)written < out_len;
}

static bool zip_join_archive_name(char *out, size_t out_len, const char *dir, const char *name)
{
    if (out == NULL || out_len == 0 || dir == NULL || name == NULL) {
        return false;
    }

    const size_t dir_len = strlen(dir);
    const int written = (dir_len > 0 && dir[dir_len - 1] == '/') ?
        snprintf(out, out_len, "%s%s", dir, name) :
        snprintf(out, out_len, "%s/%s", dir, name);
    return written >= 0 && (size_t)written < out_len;
}

static bool zip_name_is_safe(const char *name)
{
    if (name == NULL || name[0] == '\0' || name[0] == '/' || name[0] == '\\') {
        return false;
    }

    const char *segment = name;
    while (*segment != '\0') {
        const char *slash = strchr(segment, '/');
        const size_t len = slash != NULL ? (size_t)(slash - segment) : strlen(segment);

        if (len == 0) {
            return slash != NULL && slash[1] == '\0';
        }
        if ((len == 1 && segment[0] == '.') ||
            (len == 2 && segment[0] == '.' && segment[1] == '.') ||
            memchr(segment, '\\', len) != NULL ||
            memchr(segment, ':', len) != NULL) {
            return false;
        }

        if (slash == NULL) {
            break;
        }
        segment = slash + 1;
    }

    return true;
}

static esp_err_t zip_mkdir_p(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }

    char work[SOLAR_OS_STORAGE_PATH_MAX];
    char mount[SOLAR_OS_STORAGE_MOUNT_POINT_MAX];
    if (strlcpy(work, path, sizeof(work)) >= sizeof(work) ||
        solar_os_storage_path_mount_point(work, mount, sizeof(mount)) != ESP_OK) {
        errno = ENAMETOOLONG;
        return ESP_ERR_INVALID_SIZE;
    }

    size_t len = strlen(work);
    while (len > strlen(mount) && work[len - 1] == '/') {
        work[--len] = '\0';
    }

    const size_t mount_len = strlen(mount);
    for (char *cursor = work + mount_len; *cursor != '\0'; cursor++) {
        if (*cursor != '/') {
            continue;
        }
        *cursor = '\0';
        if (strlen(work) > mount_len && mkdir(work, 0777) != 0 && errno != EEXIST) {
            const int saved_errno = errno;
            *cursor = '/';
            errno = saved_errno;
            return ESP_FAIL;
        }
        *cursor = '/';
    }

    if (strlen(work) > mount_len && mkdir(work, 0777) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t zip_mkdir_parent(const char *path)
{
    char parent[SOLAR_OS_STORAGE_PATH_MAX];
    if (path == NULL || strlcpy(parent, path, sizeof(parent)) >= sizeof(parent)) {
        errno = ENAMETOOLONG;
        return ESP_ERR_INVALID_SIZE;
    }

    char *slash = strrchr(parent, '/');
    if (slash == NULL) {
        return ESP_OK;
    }

    char mount[SOLAR_OS_STORAGE_MOUNT_POINT_MAX];
    if (solar_os_storage_path_mount_point(parent, mount, sizeof(mount)) != ESP_OK) {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }
    if ((size_t)(slash - parent) <= strlen(mount)) {
        return ESP_OK;
    }

    *slash = '\0';
    return zip_mkdir_p(parent);
}

static void zip_report(zip_writer_t *writer,
                       solar_os_zip_event_t event,
                       const char *archive_name,
                       const char *path,
                       uint16_t method,
                       uint32_t compressed_size,
                       uint32_t uncompressed_size)
{
    if (writer == NULL || writer->options == NULL || writer->options->progress == NULL) {
        return;
    }

    const solar_os_zip_event_info_t info = {
        .event = event,
        .archive_name = archive_name,
        .path = path,
        .compressed_size = compressed_size,
        .uncompressed_size = uncompressed_size,
        .method = method,
    };
    writer->options->progress(&info, writer->options->user);
}

static void zip_report_read(const solar_os_unzip_options_t *options,
                            solar_os_zip_event_t event,
                            const char *archive_name,
                            const char *path,
                            uint16_t method,
                            uint32_t compressed_size,
                            uint32_t uncompressed_size)
{
    if (options == NULL || options->progress == NULL) {
        return;
    }

    const solar_os_zip_event_info_t info = {
        .event = event,
        .archive_name = archive_name,
        .path = path,
        .compressed_size = compressed_size,
        .uncompressed_size = uncompressed_size,
        .method = method,
    };
    options->progress(&info, options->user);
}

static esp_err_t zip_writer_add_meta(zip_writer_t *writer, const zip_entry_meta_t *meta)
{
    if (writer->entry_count == writer->entry_capacity) {
        const size_t new_capacity = writer->entry_capacity == 0 ? 16 : writer->entry_capacity * 2;
        zip_entry_meta_t *new_entries = zip_malloc(new_capacity * sizeof(*new_entries));
        if (new_entries == NULL) {
            return ESP_ERR_NO_MEM;
        }
        if (writer->entries != NULL) {
            memcpy(new_entries, writer->entries, writer->entry_count * sizeof(*new_entries));
            heap_caps_free(writer->entries);
        }
        writer->entries = new_entries;
        writer->entry_capacity = new_capacity;
    }

    writer->entries[writer->entry_count++] = *meta;
    return ESP_OK;
}

static bool zip_write_local_header(FILE *file,
                                   const char *name,
                                   uint16_t method,
                                   uint32_t crc32,
                                   uint32_t compressed_size,
                                   uint32_t uncompressed_size)
{
    const size_t name_len = strlen(name);
    if (name_len > UINT16_MAX) {
        return false;
    }

    return zip_write_u32(file, ZIP_LOCAL_FILE_HEADER_SIGNATURE) &&
           zip_write_u16(file, method == ZIP_METHOD_DEFLATE ? 20 : 10) &&
           zip_write_u16(file, 0) &&
           zip_write_u16(file, method) &&
           zip_write_u16(file, 0) &&
           zip_write_u16(file, 0) &&
           zip_write_u32(file, crc32) &&
           zip_write_u32(file, compressed_size) &&
           zip_write_u32(file, uncompressed_size) &&
           zip_write_u16(file, (uint16_t)name_len) &&
           zip_write_u16(file, 0) &&
           zip_write_all(file, name, name_len);
}

static bool zip_patch_local_header(FILE *file,
                                   uint32_t local_offset,
                                   uint32_t crc32,
                                   uint32_t compressed_size,
                                   uint32_t uncompressed_size)
{
    if (fseek(file, (long)local_offset + 14L, SEEK_SET) != 0) {
        return false;
    }
    return zip_write_u32(file, crc32) &&
           zip_write_u32(file, compressed_size) &&
           zip_write_u32(file, uncompressed_size) &&
           fseek(file, 0, SEEK_END) == 0;
}

static esp_err_t zip_store_file(zip_writer_t *writer,
                                FILE *source,
                                uint32_t *crc32,
                                uint32_t *compressed_size,
                                uint32_t *uncompressed_size)
{
    *crc32 = MZ_CRC32_INIT;
    *compressed_size = 0;
    *uncompressed_size = 0;

    while (true) {
        const size_t bytes_read = fread(writer->in_buffer, 1, ZIP_IO_BUFFER_SIZE, source);
        if (bytes_read > 0) {
            if (*uncompressed_size > UINT32_MAX - bytes_read ||
                *compressed_size > UINT32_MAX - bytes_read) {
                return ESP_ERR_INVALID_SIZE;
            }
            *crc32 = (uint32_t)mz_crc32(*crc32, writer->in_buffer, bytes_read);
            if (!zip_write_all(writer->file, writer->in_buffer, bytes_read)) {
                return ESP_FAIL;
            }
            *uncompressed_size += (uint32_t)bytes_read;
            *compressed_size += (uint32_t)bytes_read;
            zip_yield();
        }

        if (bytes_read < ZIP_IO_BUFFER_SIZE) {
            return ferror(source) ? ESP_FAIL : ESP_OK;
        }
    }
}

static esp_err_t zip_deflate_file(zip_writer_t *writer,
                                  FILE *source,
                                  uint32_t *crc32,
                                  uint32_t *compressed_size,
                                  uint32_t *uncompressed_size)
{
    tdefl_compressor *compressor = zip_calloc(1, sizeof(*compressor));
    if (compressor == NULL) {
        return ESP_ERR_NO_MEM;
    }

    tdefl_status status =
        tdefl_init(compressor, NULL, NULL, TDEFL_DEFAULT_MAX_PROBES | TDEFL_GREEDY_PARSING_FLAG);
    if (status != TDEFL_STATUS_OKAY) {
        heap_caps_free(compressor);
        return ESP_FAIL;
    }

    *crc32 = MZ_CRC32_INIT;
    *compressed_size = 0;
    *uncompressed_size = 0;

    bool eof = false;
    do {
        const size_t bytes_read = fread(writer->in_buffer, 1, ZIP_IO_BUFFER_SIZE, source);
        if (bytes_read < ZIP_IO_BUFFER_SIZE) {
            if (ferror(source)) {
                heap_caps_free(compressor);
                return ESP_FAIL;
            }
            eof = true;
        }

        if (*uncompressed_size > UINT32_MAX - bytes_read) {
            heap_caps_free(compressor);
            return ESP_ERR_INVALID_SIZE;
        }
        *crc32 = (uint32_t)mz_crc32(*crc32, writer->in_buffer, bytes_read);
        *uncompressed_size += (uint32_t)bytes_read;

        size_t input_pos = 0;
        do {
            size_t in_bytes = bytes_read - input_pos;
            size_t out_bytes = TDEFL_OUT_BUF_SIZE;
            status = tdefl_compress(compressor,
                                    &writer->in_buffer[input_pos],
                                    &in_bytes,
                                    writer->out_buffer,
                                    &out_bytes,
                                    eof ? TDEFL_FINISH : TDEFL_NO_FLUSH);
            input_pos += in_bytes;

            if (out_bytes > 0) {
                if (*compressed_size > UINT32_MAX - out_bytes) {
                    heap_caps_free(compressor);
                    return ESP_ERR_INVALID_SIZE;
                }
                if (!zip_write_all(writer->file, writer->out_buffer, out_bytes)) {
                    heap_caps_free(compressor);
                    return ESP_FAIL;
                }
                *compressed_size += (uint32_t)out_bytes;
            }

            if (status < TDEFL_STATUS_OKAY) {
                heap_caps_free(compressor);
                return ESP_FAIL;
            }
            if (status == TDEFL_STATUS_OKAY && in_bytes == 0 && out_bytes == 0 && !eof) {
                break;
            }
        } while (input_pos < bytes_read || (eof && status != TDEFL_STATUS_DONE));
        zip_yield();
    } while (!eof || status != TDEFL_STATUS_DONE);

    heap_caps_free(compressor);
    return ESP_OK;
}

static esp_err_t zip_add_directory_entry(zip_writer_t *writer, const char *archive_name)
{
    char dir_name[ZIP_NAME_MAX];
    const size_t name_len = strlen(archive_name);
    if (name_len + 1 >= sizeof(dir_name)) {
        return ESP_ERR_INVALID_SIZE;
    }
    strlcpy(dir_name, archive_name, sizeof(dir_name));
    if (name_len == 0 || dir_name[name_len - 1] != '/') {
        dir_name[name_len] = '/';
        dir_name[name_len + 1] = '\0';
    }

    zip_entry_meta_t meta = {
        .method = ZIP_METHOD_STORE,
        .directory = true,
    };
    if (strlcpy(meta.name, dir_name, sizeof(meta.name)) >= sizeof(meta.name) ||
        !zip_tell_u32(writer->file, &meta.local_offset) ||
        !zip_write_local_header(writer->file, meta.name, meta.method, 0, 0, 0)) {
        return ESP_FAIL;
    }

    esp_err_t ret = zip_writer_add_meta(writer, &meta);
    if (ret == ESP_OK) {
        zip_report(writer, SOLAR_OS_ZIP_EVENT_DIRECTORY, meta.name, NULL, meta.method, 0, 0);
    }
    return ret;
}

static esp_err_t zip_add_file_entry(zip_writer_t *writer,
                                    const char *source_path,
                                    const char *archive_name)
{
    if (strcmp(source_path, writer->archive_path) == 0) {
        return ESP_OK;
    }

    FILE *source = fopen(source_path, "rb");
    if (source == NULL) {
        return ESP_FAIL;
    }

    zip_entry_meta_t meta = {
        .method = writer->options != NULL && writer->options->store_only ?
            ZIP_METHOD_STORE :
            ZIP_METHOD_DEFLATE,
        .directory = false,
    };
    if (strlcpy(meta.name, archive_name, sizeof(meta.name)) >= sizeof(meta.name) ||
        !zip_tell_u32(writer->file, &meta.local_offset) ||
        !zip_write_local_header(writer->file, meta.name, meta.method, 0, 0, 0)) {
        fclose(source);
        return ESP_FAIL;
    }

    esp_err_t ret = meta.method == ZIP_METHOD_STORE ?
        zip_store_file(writer,
                       source,
                       &meta.crc32,
                       &meta.compressed_size,
                       &meta.uncompressed_size) :
        zip_deflate_file(writer,
                         source,
                         &meta.crc32,
                         &meta.compressed_size,
                         &meta.uncompressed_size);
    const int source_errno = errno;
    fclose(source);
    if (ret != ESP_OK) {
        errno = source_errno;
        return ret;
    }

    if (!zip_patch_local_header(writer->file,
                                meta.local_offset,
                                meta.crc32,
                                meta.compressed_size,
                                meta.uncompressed_size)) {
        return ESP_FAIL;
    }

    ret = zip_writer_add_meta(writer, &meta);
    if (ret == ESP_OK) {
        zip_report(writer,
                   SOLAR_OS_ZIP_EVENT_ADD,
                   meta.name,
                   source_path,
                   meta.method,
                   meta.compressed_size,
                   meta.uncompressed_size);
    }
    return ret;
}

static esp_err_t zip_add_path(zip_writer_t *writer,
                              const char *source_path,
                              const char *archive_name)
{
    if (!zip_name_is_safe(archive_name)) {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat st;
    if (stat(source_path, &st) != 0) {
        return ESP_FAIL;
    }

    if (!S_ISDIR(st.st_mode)) {
        return zip_add_file_entry(writer, source_path, archive_name);
    }

    esp_err_t ret = zip_add_directory_entry(writer, archive_name);
    if (ret != ESP_OK) {
        return ret;
    }

    DIR *dir = opendir(source_path);
    if (dir == NULL) {
        return ESP_FAIL;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child_path[SOLAR_OS_STORAGE_PATH_MAX];
        char child_name[ZIP_NAME_MAX];
        if (!zip_join_path(child_path, sizeof(child_path), source_path, entry->d_name) ||
            !zip_join_archive_name(child_name, sizeof(child_name), archive_name, entry->d_name)) {
            ret = ESP_ERR_INVALID_SIZE;
            break;
        }

        ret = zip_add_path(writer, child_path, child_name);
        if (ret != ESP_OK) {
            break;
        }
        zip_yield();
    }

    const int saved_errno = errno;
    closedir(dir);
    errno = saved_errno;
    return ret;
}

static bool zip_write_central_entry(FILE *file, const zip_entry_meta_t *entry)
{
    const size_t name_len = strlen(entry->name);
    const uint32_t external_attr = entry->directory ? 0x00100000UL : 0;
    if (name_len > UINT16_MAX) {
        return false;
    }

    return zip_write_u32(file, ZIP_CENTRAL_DIRECTORY_SIGNATURE) &&
           zip_write_u16(file, 20) &&
           zip_write_u16(file, entry->method == ZIP_METHOD_DEFLATE ? 20 : 10) &&
           zip_write_u16(file, 0) &&
           zip_write_u16(file, entry->method) &&
           zip_write_u16(file, 0) &&
           zip_write_u16(file, 0) &&
           zip_write_u32(file, entry->crc32) &&
           zip_write_u32(file, entry->compressed_size) &&
           zip_write_u32(file, entry->uncompressed_size) &&
           zip_write_u16(file, (uint16_t)name_len) &&
           zip_write_u16(file, 0) &&
           zip_write_u16(file, 0) &&
           zip_write_u16(file, 0) &&
           zip_write_u16(file, 0) &&
           zip_write_u32(file, external_attr) &&
           zip_write_u32(file, entry->local_offset) &&
           zip_write_all(file, entry->name, name_len);
}

static esp_err_t zip_writer_finish(zip_writer_t *writer)
{
    uint32_t central_offset = 0;
    uint32_t central_end = 0;
    if (!zip_tell_u32(writer->file, &central_offset)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (writer->entry_count > UINT16_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t i = 0; i < writer->entry_count; i++) {
        if (!zip_write_central_entry(writer->file, &writer->entries[i])) {
            return ESP_FAIL;
        }
        zip_yield();
    }

    if (!zip_tell_u32(writer->file, &central_end) || central_end < central_offset) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t central_size = central_end - central_offset;

    return zip_write_u32(writer->file, ZIP_END_OF_CENTRAL_DIRECTORY_SIGNATURE) &&
           zip_write_u16(writer->file, 0) &&
           zip_write_u16(writer->file, 0) &&
           zip_write_u16(writer->file, (uint16_t)writer->entry_count) &&
           zip_write_u16(writer->file, (uint16_t)writer->entry_count) &&
           zip_write_u32(writer->file, central_size) &&
           zip_write_u32(writer->file, central_offset) &&
           zip_write_u16(writer->file, 0) ? ESP_OK : ESP_FAIL;
}

static esp_err_t zip_reader_open(zip_reader_t *reader, const char *archive_path)
{
    memset(reader, 0, sizeof(*reader));

    reader->file = fopen(archive_path, "rb");
    if (reader->file == NULL) {
        return ESP_FAIL;
    }

    if (fseek(reader->file, 0, SEEK_END) != 0) {
        fclose(reader->file);
        reader->file = NULL;
        return ESP_FAIL;
    }

    const long file_size = ftell(reader->file);
    if (file_size < (long)ZIP_EOCD_LEN) {
        fclose(reader->file);
        reader->file = NULL;
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t search_len = (size_t)((file_size < (long)ZIP_EOCD_SEARCH_MAX) ?
        file_size :
        ZIP_EOCD_SEARCH_MAX);
    uint8_t *tail = zip_malloc(search_len);
    if (tail == NULL) {
        fclose(reader->file);
        reader->file = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (fseek(reader->file, file_size - (long)search_len, SEEK_SET) != 0 ||
        fread(tail, 1, search_len, reader->file) != search_len) {
        heap_caps_free(tail);
        fclose(reader->file);
        reader->file = NULL;
        return ESP_FAIL;
    }

    uint8_t *eocd = NULL;
    for (size_t pos = search_len - ZIP_EOCD_LEN + 1; pos > 0; pos--) {
        uint8_t *candidate = &tail[pos - 1];
        if (zip_read_le32(candidate) == ZIP_END_OF_CENTRAL_DIRECTORY_SIGNATURE) {
            const uint16_t comment_len = zip_read_le16(&candidate[20]);
            if ((pos - 1) + ZIP_EOCD_LEN + comment_len == search_len) {
                eocd = candidate;
                break;
            }
        }
    }

    if (eocd == NULL) {
        heap_caps_free(tail);
        fclose(reader->file);
        reader->file = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    const uint16_t disk = zip_read_le16(&eocd[4]);
    const uint16_t central_disk = zip_read_le16(&eocd[6]);
    const uint16_t disk_entries = zip_read_le16(&eocd[8]);
    const uint16_t total_entries = zip_read_le16(&eocd[10]);
    reader->central_size = zip_read_le32(&eocd[12]);
    reader->central_offset = zip_read_le32(&eocd[16]);

    heap_caps_free(tail);

    if (disk != 0 || central_disk != 0 || disk_entries != total_entries ||
        total_entries == UINT16_MAX ||
        reader->central_size == UINT32_MAX ||
        reader->central_offset == UINT32_MAX ||
        (uint64_t)reader->central_offset + reader->central_size > (uint64_t)file_size) {
        fclose(reader->file);
        reader->file = NULL;
        return ESP_ERR_NOT_SUPPORTED;
    }

    reader->entry_count = total_entries;
    return ESP_OK;
}

static void zip_reader_close(zip_reader_t *reader)
{
    if (reader != NULL && reader->file != NULL) {
        fclose(reader->file);
        reader->file = NULL;
    }
}

static esp_err_t zip_reader_read_central_entry(FILE *file, zip_reader_entry_t *entry)
{
    uint8_t header[ZIP_CENTRAL_HEADER_LEN];
    if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
        return ESP_FAIL;
    }
    if (zip_read_le32(header) != ZIP_CENTRAL_DIRECTORY_SIGNATURE) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint16_t name_len = zip_read_le16(&header[28]);
    const uint16_t extra_len = zip_read_le16(&header[30]);
    const uint16_t comment_len = zip_read_le16(&header[32]);
    if (name_len == 0 || name_len >= sizeof(entry->name)) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(entry, 0, sizeof(*entry));
    entry->flags = zip_read_le16(&header[8]);
    entry->method = zip_read_le16(&header[10]);
    entry->crc32 = zip_read_le32(&header[16]);
    entry->compressed_size = zip_read_le32(&header[20]);
    entry->uncompressed_size = zip_read_le32(&header[24]);
    entry->local_offset = zip_read_le32(&header[42]);

    if (fread(entry->name, 1, name_len, file) != name_len) {
        return ESP_FAIL;
    }
    entry->name[name_len] = '\0';
    entry->directory = entry->name[name_len - 1] == '/';

    if (fseek(file, (long)extra_len + comment_len, SEEK_CUR) != 0) {
        return ESP_FAIL;
    }

    if (!zip_name_is_safe(entry->name)) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((entry->flags & ZIP_GP_FLAG_ENCRYPTED) != 0 ||
        (entry->method != ZIP_METHOD_STORE && entry->method != ZIP_METHOD_DEFLATE)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_OK;
}

static esp_err_t zip_skip_local_header(FILE *file, const zip_reader_entry_t *entry)
{
    uint8_t header[ZIP_LOCAL_HEADER_LEN];
    if (fseek(file, entry->local_offset, SEEK_SET) != 0 ||
        fread(header, 1, sizeof(header), file) != sizeof(header)) {
        return ESP_FAIL;
    }
    if (zip_read_le32(header) != ZIP_LOCAL_FILE_HEADER_SIGNATURE) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint16_t flags = zip_read_le16(&header[6]);
    const uint16_t method = zip_read_le16(&header[8]);
    const uint16_t name_len = zip_read_le16(&header[26]);
    const uint16_t extra_len = zip_read_le16(&header[28]);
    if ((flags & ZIP_GP_FLAG_ENCRYPTED) != 0 || method != entry->method) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    return fseek(file, (long)name_len + extra_len, SEEK_CUR) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t zip_copy_stored(FILE *archive,
                                 FILE *output,
                                 const zip_reader_entry_t *entry,
                                 uint8_t *buffer,
                                 uint32_t *crc32,
                                 uint32_t *bytes_written)
{
    uint32_t remaining = entry->compressed_size;
    *crc32 = MZ_CRC32_INIT;
    *bytes_written = 0;

    while (remaining > 0) {
        const size_t request = remaining > ZIP_IO_BUFFER_SIZE ? ZIP_IO_BUFFER_SIZE : remaining;
        const size_t bytes_read = fread(buffer, 1, request, archive);
        if (bytes_read != request) {
            return ESP_FAIL;
        }
        if (!zip_write_all(output, buffer, bytes_read)) {
            return ESP_FAIL;
        }
        *crc32 = (uint32_t)mz_crc32(*crc32, buffer, bytes_read);
        *bytes_written += (uint32_t)bytes_read;
        remaining -= (uint32_t)bytes_read;
        zip_yield();
    }
    return ESP_OK;
}

static esp_err_t zip_inflate_file(FILE *archive,
                                  FILE *output,
                                  const zip_reader_entry_t *entry,
                                  uint8_t *input,
                                  uint8_t *dict,
                                  uint32_t *crc32,
                                  uint32_t *bytes_written)
{
    tinfl_decompressor *decompressor = zip_calloc(1, sizeof(*decompressor));
    if (decompressor == NULL) {
        return ESP_ERR_NO_MEM;
    }
    tinfl_init(decompressor);

    uint32_t compressed_left = entry->compressed_size;
    size_t input_pos = 0;
    size_t input_len = 0;
    size_t dict_pos = 0;
    tinfl_status status = TINFL_STATUS_NEEDS_MORE_INPUT;
    *crc32 = MZ_CRC32_INIT;
    *bytes_written = 0;

    while (status != TINFL_STATUS_DONE) {
        if (input_pos == input_len && compressed_left > 0) {
            const size_t request = compressed_left > ZIP_IO_BUFFER_SIZE ?
                ZIP_IO_BUFFER_SIZE :
                compressed_left;
            input_len = fread(input, 1, request, archive);
            if (input_len != request) {
                heap_caps_free(decompressor);
                return ESP_FAIL;
            }
            input_pos = 0;
            compressed_left -= (uint32_t)input_len;
        }

        size_t in_bytes = input_len - input_pos;
        size_t out_bytes = ZIP_INFLATE_DICT_SIZE - dict_pos;
        const uint32_t flags = compressed_left > 0 ? TINFL_FLAG_HAS_MORE_INPUT : 0;
        status = tinfl_decompress(decompressor,
                                  &input[input_pos],
                                  &in_bytes,
                                  dict,
                                  &dict[dict_pos],
                                  &out_bytes,
                                  flags);
        input_pos += in_bytes;

        if (out_bytes > 0) {
            if (*bytes_written > UINT32_MAX - out_bytes) {
                heap_caps_free(decompressor);
                return ESP_ERR_INVALID_SIZE;
            }
            if (!zip_write_all(output, &dict[dict_pos], out_bytes)) {
                heap_caps_free(decompressor);
                return ESP_FAIL;
            }
            *crc32 = (uint32_t)mz_crc32(*crc32, &dict[dict_pos], out_bytes);
            *bytes_written += (uint32_t)out_bytes;
            dict_pos = (dict_pos + out_bytes) & (ZIP_INFLATE_DICT_SIZE - 1U);
        }

        if (status < TINFL_STATUS_DONE) {
            heap_caps_free(decompressor);
            return ESP_FAIL;
        }
        if (status == TINFL_STATUS_NEEDS_MORE_INPUT &&
            input_pos == input_len &&
            compressed_left == 0) {
            heap_caps_free(decompressor);
            return ESP_FAIL;
        }
        zip_yield();
    }

    heap_caps_free(decompressor);
    return ESP_OK;
}

static esp_err_t zip_extract_entry(zip_reader_t *reader,
                                   const zip_reader_entry_t *entry,
                                   const char *dest_dir,
                                   const solar_os_unzip_options_t *options,
                                   uint8_t *input,
                                   uint8_t *dict)
{
    char out_path[SOLAR_OS_STORAGE_PATH_MAX];
    if (!zip_join_path(out_path, sizeof(out_path), dest_dir, entry->name)) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (entry->directory) {
        const esp_err_t ret = zip_mkdir_p(out_path);
        if (ret == ESP_OK) {
            zip_report_read(options,
                            SOLAR_OS_ZIP_EVENT_DIRECTORY,
                            entry->name,
                            out_path,
                            entry->method,
                            entry->compressed_size,
                            entry->uncompressed_size);
        }
        return ret;
    }

    esp_err_t ret = zip_mkdir_parent(out_path);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = zip_skip_local_header(reader->file, entry);
    if (ret != ESP_OK) {
        return ret;
    }

    FILE *output = fopen(out_path, "wb");
    if (output == NULL) {
        return ESP_FAIL;
    }

    uint32_t crc32 = 0;
    uint32_t bytes_written = 0;
    ret = entry->method == ZIP_METHOD_STORE ?
        zip_copy_stored(reader->file, output, entry, input, &crc32, &bytes_written) :
        zip_inflate_file(reader->file, output, entry, input, dict, &crc32, &bytes_written);

    const int output_errno = errno;
    if (fclose(output) != 0 && ret == ESP_OK) {
        ret = ESP_FAIL;
    }
    if (ret != ESP_OK) {
        errno = output_errno;
        remove(out_path);
        return ret;
    }

    if (crc32 != entry->crc32 || bytes_written != entry->uncompressed_size) {
        remove(out_path);
        return ESP_ERR_INVALID_CRC;
    }

    zip_report_read(options,
                    SOLAR_OS_ZIP_EVENT_EXTRACT,
                    entry->name,
                    out_path,
                    entry->method,
                    entry->compressed_size,
                    entry->uncompressed_size);
    return ESP_OK;
}

esp_err_t solar_os_zip_create(const char *archive_path,
                              const char * const *source_paths,
                              size_t source_count,
                              const solar_os_zip_options_t *options)
{
    if (archive_path == NULL || archive_path[0] == '\0' ||
        source_paths == NULL || source_count == 0) {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }

    zip_writer_t writer = {
        .archive_path = archive_path,
        .options = options,
    };
    writer.in_buffer = zip_malloc(ZIP_IO_BUFFER_SIZE);
    writer.out_buffer = zip_malloc(TDEFL_OUT_BUF_SIZE);
    if (writer.in_buffer == NULL || writer.out_buffer == NULL) {
        heap_caps_free(writer.in_buffer);
        heap_caps_free(writer.out_buffer);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = zip_mkdir_parent(archive_path);
    if (ret == ESP_OK) {
        writer.file = fopen(archive_path, "wb");
        ret = writer.file != NULL ? ESP_OK : ESP_FAIL;
    }

    for (size_t i = 0; ret == ESP_OK && i < source_count; i++) {
        if (source_paths[i] == NULL || source_paths[i][0] == '\0') {
            ret = ESP_ERR_INVALID_ARG;
            break;
        }

        const char *base = zip_path_basename(source_paths[i]);
        if (base[0] == '\0') {
            ret = ESP_ERR_INVALID_ARG;
            break;
        }
        ret = zip_add_path(&writer, source_paths[i], base);
    }

    if (ret == ESP_OK) {
        ret = zip_writer_finish(&writer);
    }

    const int saved_errno = errno;
    if (writer.file != NULL && fclose(writer.file) != 0 && ret == ESP_OK) {
        ret = ESP_FAIL;
    }
    if (ret != ESP_OK) {
        remove(archive_path);
    }
    errno = saved_errno;
    heap_caps_free(writer.entries);
    heap_caps_free(writer.in_buffer);
    heap_caps_free(writer.out_buffer);
    return ret;
}

esp_err_t solar_os_zip_list(const char *archive_path,
                            solar_os_zip_progress_cb_t progress,
                            void *user)
{
    if (archive_path == NULL || archive_path[0] == '\0') {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }

    zip_reader_t reader;
    esp_err_t ret = zip_reader_open(&reader, archive_path);
    if (ret != ESP_OK) {
        return ret;
    }

    if (fseek(reader.file, reader.central_offset, SEEK_SET) != 0) {
        zip_reader_close(&reader);
        return ESP_FAIL;
    }

    const solar_os_unzip_options_t options = {
        .progress = progress,
        .user = user,
    };
    for (uint16_t i = 0; i < reader.entry_count; i++) {
        zip_reader_entry_t entry;
        ret = zip_reader_read_central_entry(reader.file, &entry);
        if (ret != ESP_OK) {
            break;
        }
        zip_report_read(&options,
                        SOLAR_OS_ZIP_EVENT_LIST,
                        entry.name,
                        NULL,
                        entry.method,
                        entry.compressed_size,
                        entry.uncompressed_size);
        zip_yield();
    }

    zip_reader_close(&reader);
    return ret;
}

esp_err_t solar_os_zip_extract(const char *archive_path,
                               const char *dest_dir,
                               const solar_os_unzip_options_t *options)
{
    if (archive_path == NULL || archive_path[0] == '\0' ||
        dest_dir == NULL || dest_dir[0] == '\0') {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = zip_mkdir_p(dest_dir);
    if (ret != ESP_OK) {
        return ret;
    }

    zip_reader_t reader;
    ret = zip_reader_open(&reader, archive_path);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t *input = zip_malloc(ZIP_IO_BUFFER_SIZE);
    uint8_t *dict = zip_malloc(ZIP_INFLATE_DICT_SIZE);
    if (input == NULL || dict == NULL) {
        heap_caps_free(input);
        heap_caps_free(dict);
        zip_reader_close(&reader);
        return ESP_ERR_NO_MEM;
    }

    if (fseek(reader.file, reader.central_offset, SEEK_SET) != 0) {
        ret = ESP_FAIL;
    }

    for (uint16_t i = 0; ret == ESP_OK && i < reader.entry_count; i++) {
        const long next_central = ftell(reader.file);
        zip_reader_entry_t entry;
        ret = zip_reader_read_central_entry(reader.file, &entry);
        const long after_central = ftell(reader.file);
        if (ret != ESP_OK) {
            break;
        }
        ret = zip_extract_entry(&reader, &entry, dest_dir, options, input, dict);
        if (ret != ESP_OK) {
            break;
        }
        if (next_central < 0 || after_central < 0 || fseek(reader.file, after_central, SEEK_SET) != 0) {
            ret = ESP_FAIL;
            break;
        }
        zip_yield();
    }

    heap_caps_free(input);
    heap_caps_free(dict);
    zip_reader_close(&reader);
    return ret;
}
