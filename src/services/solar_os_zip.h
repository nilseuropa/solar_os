#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_zip_types_v1.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool store_only;
    solar_os_zip_progress_cb_t progress;
    void *user;
} solar_os_zip_options_t;

typedef struct {
    solar_os_zip_progress_cb_t progress;
    void *user;
    /* Checked between entries and while copying/decompressing data. */
    const volatile bool *cancel_flag;
    bool (*cancelled)(void *user);
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
esp_err_t solar_os_zip_read_into(const char *archive_path, const char *entry_name,
                                 void *data, size_t capacity, size_t *out_len);
void solar_os_zip_free(uint8_t *data);

#ifdef __cplusplus
}
#endif
