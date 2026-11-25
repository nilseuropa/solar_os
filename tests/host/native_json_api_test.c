#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_native_client.h"
#include "solar_os_native_json.h"
#include "solar_os_memory.h"
static unsigned allocations;
static int fail_after = -1;
size_t strlcpy(char *out, const char *in, size_t capacity)
{ size_t size = strlen(in); if (capacity) { size_t n = size < capacity - 1 ? size : capacity - 1; memcpy(out, in, n); out[n] = 0; } return size; }
void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t cls, const char *tag)
{
    assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && tag);
    if (fail_after == 0) return NULL;
    if (fail_after > 0) --fail_after;
    void *p = malloc(size); if (p) ++allocations; return p;
}
void solar_os_memory_free(void *p) { if (p) --allocations; free(p); }
static const solar_os_native_json_api_v1_t *api;
static solar_os_native_json_document_t *parse(const char *text)
{
    solar_os_native_json_document_t *document; size_t error = 99;
    assert(!api->document_parse(text, strlen(text), &document, &error) && document && error == 0);
    return document;
}
int main(void)
{
    api = solar_os_native_json_get_service("json", 1, offsetof(solar_os_native_json_api_v1_t, document_parse)); assert(api);
    assert(solar_os_native_json_has_document_parse(api) && solar_os_native_json_has_array_size(api));
    solar_os_native_json_value_t *strict = NULL;
    assert(api->parse("1", 1, &strict) && !strict);
    const char *duplicates = "{\"x\":1,\"x\":2}"; assert(api->parse(duplicates, strlen(duplicates), &strict) && !strict);
    assert(!api->parse("{\"x\":1}", 7, &strict)); assert(api->kind(strict) == SOLAR_OS_NATIVE_JSON_OBJECT);
    uint64_t unsigned_value; assert(api->uint_value(api->get(strict, "x"), &unsigned_value) && unsigned_value == 1); api->free(strict);
    char source[] = "{\"node\":{\"list\":[{\"flag\":false},{\"flag\":true}]},\"words\":[\"one\",\"two\"],\"text\":\"a\\nb\",\"n\":-17}";
    solar_os_native_json_document_t *document = parse(source); const solar_os_native_json_value_t *root = api->document_root(document);
    memset(source, '?', strlen(source)); bool flag; int64_t integer; char text[16];
    assert(!api->path_bool(root, "node.list[1].flag", &flag) && flag);
    assert(!api->path_int64(root, "n", &integer) && integer == -17);
    assert(!api->path_string(root, "text", text, sizeof(text)) && !strcmp(text, "a\nb"));
    assert(api->value_kind(api->path(root, "words")) == SOLAR_OS_NATIVE_JSON_ARRAY);
    assert(api->path_contains_string(root, "words", "two") && api->contains_string(api->get(root, "words"), "one"));
    assert(!api->array_get(root, 0) && !api->array_size(root));
    assert(api->array_size(api->get(root, "words")) == 2 && !strcmp(api->string(api->array_get(api->get(root, "words"), 1)), "two"));
    assert(api->path_bool(root, "missing", &flag) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND); api->document_free(document);
    document = parse("true trailing"); root = api->document_root(document);
    assert(api->value_kind(root) == SOLAR_OS_NATIVE_JSON_BOOL && !api->value_bool(root, &flag) && flag); api->document_free(document);
    document = parse("null"); assert(api->value_kind(api->document_root(document)) == SOLAR_OS_NATIVE_JSON_NULL); api->document_free(document);
    const char *numbers[] = {"9223372036854775808", "1e999", "1.5"};
    for (unsigned i = 0; i < 3; ++i) {
        document = parse(numbers[i]); assert(api->value_int64(api->document_root(document), &integer) == SOLAR_OS_NATIVE_ERROR_FAILED); api->document_free(document);
    }
    document = parse("-9223372036854775808"); assert(!api->value_int64(api->document_root(document), &integer) && integer == INT64_MIN); api->document_free(document);
    document = parse("9223372036854774784"); assert(!api->value_int64(api->document_root(document), &integer) && integer == INT64_MAX - 1023); api->document_free(document);
    document = parse("4294967295"); uint32_t value;
    assert(!api->value_uint32(api->document_root(document), &value) && value == UINT32_MAX); api->document_free(document);
    size_t error; assert(api->document_parse("{\"x\":", 5, &document, &error) && !document && error > 0 && error <= 5);
    const char *line = "{\"u\":18446744073709551615,\"s\":\"hello\",\"v\":[1,true]}";
    assert(api->scan_uint64(line, "u", &unsigned_value) && unsigned_value == UINT64_MAX);
    bool truncated; assert(api->scan_string(line, "s", text, 3, &truncated) && truncated && !strcmp(text, "he"));
    const char *slice; size_t size; assert(api->scan_value(line, "v", &slice, &size) && size == 8 && !memcmp(slice, "[1,true]", size));
    assert(slice >= line && slice + size <= line + strlen(line));
    assert(api->scan_array_value("[1,true]", 1, &slice, &size) && size == 4 && !memcmp(slice, "true", 4));
    assert(!api->escape_string("a\nb", text, sizeof(text)) && !strcmp(text, "a\\nb"));
    assert(!allocations);
    fail_after = 1; assert(api->document_parse("123", 3, &document, &error) && !document && !allocations); fail_after = -1;
    puts("native JSON: legacy prefix, OS documents, typed paths, uint64 scans and integer boundaries passed"); return 0;
}
