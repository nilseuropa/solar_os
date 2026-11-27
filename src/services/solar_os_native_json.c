#include "solar_os_native_json.h"
#include "solar_os_native_data_abi.h"
#include "solar_os_native_support.h"
#include "solar_os_json.h"
#include "cJSON.h"
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <string.h>

static bool unique_keys(const cJSON *root, unsigned depth)
{
    if (depth > 32) return false;
    const cJSON *a, *b;
    cJSON_ArrayForEach(a, root) {
        if (cJSON_IsObject(root)) for (b = a->next; b; b = b->next)
            if (!strcmp(a->string, b->string)) return false;
        if (!unique_keys(a, depth + 1)) return false;
    }
    return true;
}
static int json_parse(const char *text, size_t size, solar_os_native_json_value_t **out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = NULL;
    if (!text || !size || size > 65536 || memchr(text, 0, size)) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    unsigned depth = 0; bool quote = false, escape = false;
    for (size_t i = 0; i < size; ++i) {
        unsigned char c = text[i];
        if (i + 6 <= size && !memcmp(text + i, "\\u0000", 6)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        if (quote) {
            if (c < 32) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') quote = false;
        } else if (c == '"') quote = true;
        else if (c == '{' || c == '[') { if (++depth > 32) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE; }
        else if (c == '}' || c == ']') { if (!depth) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT; --depth; }
    }
    if (quote || depth || solar_os_json_init()) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(text, size, &end, false);
    while (end && end < text + size && isspace((unsigned char)*end)) ++end;
    if (!root || end != text + size || (!cJSON_IsObject(root) && !cJSON_IsArray(root)) || !unique_keys(root, 0)) {
        cJSON_Delete(root); return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    *out = (solar_os_native_json_value_t *)root; return 0;
}
static const solar_os_native_json_value_t *json_get(const solar_os_native_json_value_t *v, const char *key)
{ return (const void *)cJSON_GetObjectItemCaseSensitive((const cJSON *)v, key); }
static const solar_os_native_json_value_t *json_at(const solar_os_native_json_value_t *v, size_t index)
{ return index > INT_MAX ? NULL : (const void *)cJSON_GetArrayItem((const cJSON *)v, index); }
static const char *json_string(const solar_os_native_json_value_t *v)
{ return cJSON_IsString((const cJSON *)v) ? ((const cJSON *)v)->valuestring : NULL; }
static bool json_uint(const solar_os_native_json_value_t *v, uint64_t *out)
{
    const cJSON *n = (const cJSON *)v;
    if (!out || !cJSON_IsNumber(n) || !isfinite(n->valuedouble) || n->valuedouble < 0 ||
        n->valuedouble > 9007199254740991.0 || floor(n->valuedouble) != n->valuedouble) return false;
    *out = n->valuedouble; return true;
}
static size_t json_count(const solar_os_native_json_value_t *v) { return cJSON_GetArraySize((const cJSON *)v); }
static const char *json_key(const solar_os_native_json_value_t *v) { return v ? ((const cJSON *)v)->string : NULL; }
static void json_free(solar_os_native_json_value_t *v) { cJSON_Delete((cJSON *)v); }
static uint32_t json_kind(const solar_os_native_json_value_t *v)
{ return cJSON_IsObject((const cJSON *)v) ? SOLAR_OS_NATIVE_JSON_OBJECT : cJSON_IsArray((const cJSON *)v) ? SOLAR_OS_NATIVE_JSON_ARRAY : 0; }
static int document_parse(const char *text, size_t size, solar_os_native_json_document_t **out, size_t *error_offset)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = NULL; solar_os_json_doc_t *document = NULL;
    int result = solar_os_native_result(solar_os_json_parse_ex(text, size, &document, error_offset));
    *out = (solar_os_native_json_document_t *)document; return result;
}
static void document_free(solar_os_native_json_document_t *document) { solar_os_json_free((solar_os_json_doc_t *)document); }
static const solar_os_native_json_value_t *document_root(const solar_os_native_json_document_t *document)
{ return (const solar_os_native_json_value_t *)solar_os_json_root((const solar_os_json_doc_t *)document); }
static const solar_os_native_json_value_t *json_path(const solar_os_native_json_value_t *value, const char *path)
{ return (const solar_os_native_json_value_t *)solar_os_json_path_get((const solar_os_json_value_t *)value, path); }
static uint32_t value_kind(const solar_os_native_json_value_t *value)
{
    const solar_os_json_value_t *v = (const solar_os_json_value_t *)value;
    if (solar_os_json_is_object(v)) return SOLAR_OS_NATIVE_JSON_OBJECT;
    if (solar_os_json_is_array(v)) return SOLAR_OS_NATIVE_JSON_ARRAY;
    if (solar_os_json_is_string(v)) return SOLAR_OS_NATIVE_JSON_STRING;
    if (solar_os_json_is_number(v)) return SOLAR_OS_NATIVE_JSON_NUMBER;
    if (solar_os_json_is_bool(v)) return SOLAR_OS_NATIVE_JSON_BOOL;
    if (solar_os_json_is_null(v)) return SOLAR_OS_NATIVE_JSON_NULL;
    return 0;
}
static int value_string(const solar_os_native_json_value_t *value, char *text, size_t capacity)
{ return solar_os_native_result(solar_os_json_get_string((const solar_os_json_value_t *)value, text, capacity)); }
static int value_int64(const solar_os_native_json_value_t *value, int64_t *out)
{ return solar_os_native_result(solar_os_json_get_int64((const solar_os_json_value_t *)value, out)); }
static int value_uint32(const solar_os_native_json_value_t *value, uint32_t *out)
{ return solar_os_native_result(solar_os_json_get_uint32((const solar_os_json_value_t *)value, out)); }
static int value_bool(const solar_os_native_json_value_t *value, bool *out)
{ return solar_os_native_result(solar_os_json_get_bool((const solar_os_json_value_t *)value, out)); }
static int path_string(const solar_os_native_json_value_t *value, const char *path, char *text, size_t capacity)
{ return solar_os_native_result(solar_os_json_get_path_string((const solar_os_json_value_t *)value, path, text, capacity)); }
static int path_int64(const solar_os_native_json_value_t *value, const char *path, int64_t *out)
{ return solar_os_native_result(solar_os_json_get_path_int64((const solar_os_json_value_t *)value, path, out)); }
static int path_uint32(const solar_os_native_json_value_t *value, const char *path, uint32_t *out)
{ return solar_os_native_result(solar_os_json_get_path_uint32((const solar_os_json_value_t *)value, path, out)); }
static int path_bool(const solar_os_native_json_value_t *value, const char *path, bool *out)
{ return solar_os_native_result(solar_os_json_get_path_bool((const solar_os_json_value_t *)value, path, out)); }
static bool contains_string(const solar_os_native_json_value_t *array, const char *text)
{ return solar_os_json_array_contains_string((const solar_os_json_value_t *)array, text); }
static bool path_contains_string(const solar_os_native_json_value_t *value, const char *path, const char *text)
{ return solar_os_json_path_array_contains_string((const solar_os_json_value_t *)value, path, text); }
static int escape_string(const char *source, char *text, size_t capacity)
{ return solar_os_native_result(solar_os_json_escape_string(source, text, capacity)); }
static const solar_os_native_json_value_t *array_get(const solar_os_native_json_value_t *array, size_t index)
{ return (const solar_os_native_json_value_t *)solar_os_json_array_get((const solar_os_json_value_t *)array, index); }
static size_t array_size(const solar_os_native_json_value_t *array)
{ return solar_os_json_array_size((const solar_os_json_value_t *)array); }
static const solar_os_native_json_api_v1_t json_api = {
    .abi_version = SOLAR_OS_NATIVE_JSON_ABI, .struct_size = sizeof(json_api),
    .parse = json_parse, .get = json_get, .at = json_at, .string = json_string, .uint_value = json_uint,
    .count = json_count, .key = json_key, .free = json_free, .kind = json_kind,
    .document_parse = document_parse, .document_free = document_free, .document_root = document_root, .path = json_path,
    .value_kind = value_kind, .value_string = value_string, .value_int64 = value_int64, .value_uint32 = value_uint32, .value_bool = value_bool,
    .path_string = path_string, .path_int64 = path_int64, .path_uint32 = path_uint32, .path_bool = path_bool,
    .contains_string = contains_string, .path_contains_string = path_contains_string,
    .scan_string = solar_os_json_scan_object_string, .scan_uint64 = solar_os_json_scan_object_uint64,
    .scan_value = solar_os_json_scan_object_value, .scan_array_value = solar_os_json_scan_array_value, .escape_string = escape_string,
    .array_get = array_get, .array_size = array_size,
};
const void *solar_os_native_json_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_json_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
