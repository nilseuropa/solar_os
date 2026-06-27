#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_JSON_KEY_MAX 64

typedef struct solar_os_json_doc solar_os_json_doc_t;
typedef struct cJSON solar_os_json_value_t;

esp_err_t solar_os_json_parse(const char *source,
                              size_t source_len,
                              solar_os_json_doc_t **out_doc);
esp_err_t solar_os_json_parse_ex(const char *source,
                                 size_t source_len,
                                 solar_os_json_doc_t **out_doc,
                                 size_t *error_offset);
esp_err_t solar_os_json_parse_cstr(const char *source, solar_os_json_doc_t **out_doc);
void solar_os_json_free(solar_os_json_doc_t *doc);

const solar_os_json_value_t *solar_os_json_root(const solar_os_json_doc_t *doc);
const solar_os_json_value_t *solar_os_json_object_get(const solar_os_json_value_t *object,
                                                      const char *key);
const solar_os_json_value_t *solar_os_json_path_get(const solar_os_json_value_t *value,
                                                    const char *path);
size_t solar_os_json_array_size(const solar_os_json_value_t *array);
const solar_os_json_value_t *solar_os_json_array_get(const solar_os_json_value_t *array,
                                                     size_t index);

bool solar_os_json_is_object(const solar_os_json_value_t *value);
bool solar_os_json_is_array(const solar_os_json_value_t *value);
bool solar_os_json_is_string(const solar_os_json_value_t *value);
bool solar_os_json_is_number(const solar_os_json_value_t *value);
bool solar_os_json_is_bool(const solar_os_json_value_t *value);
bool solar_os_json_is_null(const solar_os_json_value_t *value);

esp_err_t solar_os_json_get_string(const solar_os_json_value_t *value,
                                   char *out,
                                   size_t out_len);
esp_err_t solar_os_json_get_int64(const solar_os_json_value_t *value, int64_t *out);
esp_err_t solar_os_json_get_uint32(const solar_os_json_value_t *value, uint32_t *out);
esp_err_t solar_os_json_get_bool(const solar_os_json_value_t *value, bool *out);

esp_err_t solar_os_json_get_path_string(const solar_os_json_value_t *value,
                                        const char *path,
                                        char *out,
                                        size_t out_len);
esp_err_t solar_os_json_get_path_int64(const solar_os_json_value_t *value,
                                       const char *path,
                                       int64_t *out);
esp_err_t solar_os_json_get_path_uint32(const solar_os_json_value_t *value,
                                        const char *path,
                                        uint32_t *out);
esp_err_t solar_os_json_get_path_bool(const solar_os_json_value_t *value,
                                      const char *path,
                                      bool *out);

bool solar_os_json_array_contains_string(const solar_os_json_value_t *array,
                                         const char *text);
bool solar_os_json_path_array_contains_string(const solar_os_json_value_t *value,
                                              const char *path,
                                              const char *text);
esp_err_t solar_os_json_escape_string(const char *source, char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
