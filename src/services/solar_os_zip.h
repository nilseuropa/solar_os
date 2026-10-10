#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SOLAR_OS_ZIP_EVENT_ADD,
    SOLAR_OS_ZIP_EVENT_EXTRACT,
    SOLAR_OS_ZIP_EVENT_DIRECTORY,
    SOLAR_OS_ZIP_EVENT_LIST,
} solar_os_zip_event_t;

typedef struct {
    solar_os_zip_event_t event;
    const char *archive_name;
    const char *path;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint16_t method;
    uint16_t flags;
    uint32_t external_attributes;
} solar_os_zip_event_info_t;

typedef void (*solar_os_zip_progress_cb_t)(const solar_os_zip_event_info_t *info, void *user);

typedef struct {
    bool store_only;
    solar_os_zip_progress_cb_t progress;
    void *user;
} solar_os_zip_options_t;

typedef struct {
    solar_os_zip_progress_cb_t progress;
    void *user;
    /* Checked between entries and between stored-file copy chunks. */
    const volatile bool *cancel_flag;
} solar_os_unzip_options_t;

esp_err_t solar_os_zip_create(const char *archive_path,
                              const char * const *source_paths,
                              size_t source_count,
                              const solar_os_zip_options_t *options);
esp_err_t solar_os_zip_list(const char *archive_path,
                            solar_os_zip_progress_cb_t progress,
                            void *user);
esp_err_t solar_os_zip_extract(const char *archive_path,
                               const char *dest_dir,
                               const solar_os_unzip_options_t *options);
esp_err_t solar_os_zip_read_file(const char *archive_path,
                                 const char *entry_name,
                                 size_t max_len,
                                 uint8_t **out_data,
                                 size_t *out_len);
void solar_os_zip_free(uint8_t *data);

#ifdef __cplusplus
}
#endif
