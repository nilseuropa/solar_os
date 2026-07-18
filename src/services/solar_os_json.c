#include "solar_os_json.h"

#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "solar_os_memory.h"

struct solar_os_json_doc {
    cJSON *root;
};

static bool json_hooks_initialized;

static void *json_malloc(size_t size)
{
    if (size == 0) {
        size = 1;
    }

    return solar_os_memory_alloc(size,
                                 SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                 "json");
}

static void json_free(void *ptr)
{
    solar_os_memory_free(ptr);
}

static void json_init_hooks(void)
{
    if (json_hooks_initialized) {
        return;
    }

    cJSON_Hooks hooks = {
        .malloc_fn = json_malloc,
        .free_fn = json_free,
    };
    cJSON_InitHooks(&hooks);
    json_hooks_initialized = true;
}

static bool json_copy_key(const char *start, size_t len, char *out, size_t out_len)
{
    if (start == NULL || out == NULL || out_len == 0 || len == 0 || len >= out_len) {
        return false;
    }

    memcpy(out, start, len);
    out[len] = '\0';
    return true;
}

esp_err_t solar_os_json_parse_ex(const char *source,
                                 size_t source_len,
                                 solar_os_json_doc_t **out_doc,
                                 size_t *error_offset)
{
    if (out_doc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_doc = NULL;
    if (error_offset != NULL) {
        *error_offset = 0;
    }
    if (source == NULL || source_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    json_init_hooks();

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(source, source_len, &parse_end, false);
    if (root == NULL) {
        if (error_offset != NULL && parse_end != NULL && parse_end >= source) {
            *error_offset = (size_t)(parse_end - source);
        }
        return ESP_FAIL;
    }

    solar_os_json_doc_t *doc = json_malloc(sizeof(*doc));
    if (doc == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    doc->root = root;
    *out_doc = doc;
    return ESP_OK;
}

esp_err_t solar_os_json_parse(const char *source,
                              size_t source_len,
                              solar_os_json_doc_t **out_doc)
{
    return solar_os_json_parse_ex(source, source_len, out_doc, NULL);
}

esp_err_t solar_os_json_parse_cstr(const char *source, solar_os_json_doc_t **out_doc)
{
    if (source == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return solar_os_json_parse(source, strlen(source), out_doc);
}

void solar_os_json_free(solar_os_json_doc_t *doc)
{
    if (doc == NULL) {
        return;
    }
    cJSON_Delete(doc->root);
    json_free(doc);
}

const solar_os_json_value_t *solar_os_json_root(const solar_os_json_doc_t *doc)
{
    return doc != NULL ? (const solar_os_json_value_t *)doc->root : NULL;
}

const solar_os_json_value_t *solar_os_json_object_get(const solar_os_json_value_t *object,
                                                      const char *key)
{
    if (object == NULL || key == NULL || key[0] == '\0' || !cJSON_IsObject((const cJSON *)object)) {
        return NULL;
    }
    return (const solar_os_json_value_t *)cJSON_GetObjectItemCaseSensitive((const cJSON *)object,
                                                                           key);
}

const solar_os_json_value_t *solar_os_json_array_get(const solar_os_json_value_t *array,
                                                     size_t index)
{
    if (array == NULL || !cJSON_IsArray((const cJSON *)array) || index > (size_t)INT_MAX) {
        return NULL;
    }
    return (const solar_os_json_value_t *)cJSON_GetArrayItem((const cJSON *)array, (int)index);
}

size_t solar_os_json_array_size(const solar_os_json_value_t *array)
{
    if (array == NULL || !cJSON_IsArray((const cJSON *)array)) {
        return 0;
    }

    const int size = cJSON_GetArraySize((const cJSON *)array);
    return size > 0 ? (size_t)size : 0;
}

const solar_os_json_value_t *solar_os_json_path_get(const solar_os_json_value_t *value,
                                                    const char *path)
{
    if (value == NULL || path == NULL) {
        return NULL;
    }
    if (path[0] == '\0') {
        return value;
    }

    const solar_os_json_value_t *current = value;
    const char *p = path;

    while (*p != '\0') {
        if (*p == '.') {
            return NULL;
        }

        const char *key_start = p;
        while (*p != '\0' && *p != '.' && *p != '[') {
            p++;
        }
        if (p > key_start) {
            char key[SOLAR_OS_JSON_KEY_MAX];
            if (!json_copy_key(key_start, (size_t)(p - key_start), key, sizeof(key))) {
                return NULL;
            }
            current = solar_os_json_object_get(current, key);
            if (current == NULL) {
                return NULL;
            }
        }

        while (*p == '[') {
            p++;
            if (!isdigit((unsigned char)*p)) {
                return NULL;
            }
            size_t index = 0;
            while (isdigit((unsigned char)*p)) {
                const size_t digit = (size_t)(*p - '0');
                if (index > ((SIZE_MAX - digit) / 10U)) {
                    return NULL;
                }
                index = (index * 10U) + digit;
                p++;
            }
            if (*p != ']') {
                return NULL;
            }
            p++;

            current = solar_os_json_array_get(current, index);
            if (current == NULL) {
                return NULL;
            }
        }

        if (*p == '.') {
            p++;
            if (*p == '\0') {
                return NULL;
            }
        } else if (*p != '\0') {
            return NULL;
        }
    }

    return current;
}

bool solar_os_json_is_object(const solar_os_json_value_t *value)
{
    return value != NULL && cJSON_IsObject((const cJSON *)value);
}

bool solar_os_json_is_array(const solar_os_json_value_t *value)
{
    return value != NULL && cJSON_IsArray((const cJSON *)value);
}

bool solar_os_json_is_string(const solar_os_json_value_t *value)
{
    return value != NULL && cJSON_IsString((const cJSON *)value);
}

bool solar_os_json_is_number(const solar_os_json_value_t *value)
{
    return value != NULL && cJSON_IsNumber((const cJSON *)value);
}

bool solar_os_json_is_bool(const solar_os_json_value_t *value)
{
    return value != NULL && cJSON_IsBool((const cJSON *)value);
}

bool solar_os_json_is_null(const solar_os_json_value_t *value)
{
    return value != NULL && cJSON_IsNull((const cJSON *)value);
}

esp_err_t solar_os_json_get_string(const solar_os_json_value_t *value,
                                   char *out,
                                   size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (value == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!cJSON_IsString((const cJSON *)value) ||
        ((const cJSON *)value)->valuestring == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const char *text = ((const cJSON *)value)->valuestring;
    if (strlcpy(out, text, out_len) >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t solar_os_json_get_int64(const solar_os_json_value_t *value, int64_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = 0;
    if (value == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!cJSON_IsNumber((const cJSON *)value)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const double number = cJSON_GetNumberValue((const cJSON *)value);
    if (number < (double)INT64_MIN || number > (double)INT64_MAX) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const int64_t integer = (int64_t)number;
    if ((double)integer != number) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out = integer;
    return ESP_OK;
}

esp_err_t solar_os_json_get_uint32(const solar_os_json_value_t *value, uint32_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = 0;

    int64_t integer = 0;
    const esp_err_t err = solar_os_json_get_int64(value, &integer);
    if (err != ESP_OK) {
        return err;
    }
    if (integer < 0 || integer > UINT32_MAX) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out = (uint32_t)integer;
    return ESP_OK;
}

esp_err_t solar_os_json_get_bool(const solar_os_json_value_t *value, bool *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = false;
    if (value == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!cJSON_IsBool((const cJSON *)value)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out = cJSON_IsTrue((const cJSON *)value);
    return ESP_OK;
}

esp_err_t solar_os_json_get_path_string(const solar_os_json_value_t *value,
                                        const char *path,
                                        char *out,
                                        size_t out_len)
{
    const solar_os_json_value_t *target = solar_os_json_path_get(value, path);
    return target != NULL ? solar_os_json_get_string(target, out, out_len) : ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_json_get_path_int64(const solar_os_json_value_t *value,
                                       const char *path,
                                       int64_t *out)
{
    const solar_os_json_value_t *target = solar_os_json_path_get(value, path);
    return target != NULL ? solar_os_json_get_int64(target, out) : ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_json_get_path_uint32(const solar_os_json_value_t *value,
                                        const char *path,
                                        uint32_t *out)
{
    const solar_os_json_value_t *target = solar_os_json_path_get(value, path);
    return target != NULL ? solar_os_json_get_uint32(target, out) : ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_json_get_path_bool(const solar_os_json_value_t *value,
                                      const char *path,
                                      bool *out)
{
    const solar_os_json_value_t *target = solar_os_json_path_get(value, path);
    return target != NULL ? solar_os_json_get_bool(target, out) : ESP_ERR_NOT_FOUND;
}

bool solar_os_json_array_contains_string(const solar_os_json_value_t *array,
                                         const char *text)
{
    if (array == NULL || text == NULL || !cJSON_IsArray((const cJSON *)array)) {
        return false;
    }

    const size_t count = solar_os_json_array_size(array);
    for (size_t i = 0; i < count; i++) {
        const solar_os_json_value_t *item = solar_os_json_array_get(array, i);
        if (item != NULL &&
            cJSON_IsString((const cJSON *)item) &&
            ((const cJSON *)item)->valuestring != NULL &&
            strcmp(((const cJSON *)item)->valuestring, text) == 0) {
            return true;
        }
    }

    return false;
}

bool solar_os_json_path_array_contains_string(const solar_os_json_value_t *value,
                                              const char *path,
                                              const char *text)
{
    return solar_os_json_array_contains_string(solar_os_json_path_get(value, path), text);
}

esp_err_t solar_os_json_escape_string(const char *source, char *out, size_t out_len)
{
    if (source == NULL || out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t written = 0;
    out[0] = '\0';

    for (const unsigned char *p = (const unsigned char *)source; *p != '\0'; p++) {
        char escaped[7];
        const char *chunk = escaped;

        switch (*p) {
        case '\"':
            chunk = "\\\"";
            break;
        case '\\':
            chunk = "\\\\";
            break;
        case '\b':
            chunk = "\\b";
            break;
        case '\f':
            chunk = "\\f";
            break;
        case '\n':
            chunk = "\\n";
            break;
        case '\r':
            chunk = "\\r";
            break;
        case '\t':
            chunk = "\\t";
            break;
        default:
            if (*p < 0x20) {
                snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned)*p);
            } else {
                escaped[0] = (char)*p;
                escaped[1] = '\0';
            }
            break;
        }

        const size_t chunk_len = strlen(chunk);
        if (written + chunk_len >= out_len) {
            out[out_len - 1U] = '\0';
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(out + written, chunk, chunk_len);
        written += chunk_len;
        out[written] = '\0';
    }

    return ESP_OK;
}
