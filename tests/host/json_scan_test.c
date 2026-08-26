#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_json_scan.h"

static void test_string_fields(void)
{
    const char *json =
        " {\"ignored\":{\"nested\":[1,{\"text\":\"skip\"}]},"
        "\"text\":\"hello\\nworld\",\"unicode\":\"x\\u263ay\"}";
    char value[32];
    assert(solar_os_json_scan_object_string(
        json, "text", value, sizeof(value), NULL));
    assert(strcmp(value, "hello\nworld") == 0);
    assert(solar_os_json_scan_object_string(
        json, "unicode", value, sizeof(value), NULL));
    assert(strcmp(value, "x?y") == 0);

    bool truncated = false;
    char short_value[5];
    assert(solar_os_json_scan_object_string(
        json, "text", short_value, sizeof(short_value), &truncated));
    assert(truncated);
    assert(strcmp(short_value, "hell") == 0);
}

static void test_invalid_strings(void)
{
    char value[16];
    assert(!solar_os_json_scan_object_string(
        "{\"value\":\"bad\\u12\"}", "value", value, sizeof(value), NULL));
    assert(!solar_os_json_scan_object_string(
        "{\"value\":\"bad\\q\"}", "value", value, sizeof(value), NULL));
    assert(!solar_os_json_scan_object_string(
        "{\"value\":\"valid\"garbage}", "value", value, sizeof(value), NULL));
    assert(!solar_os_json_scan_object_string(
        "{\"nested\":[{]}}", "missing", value, sizeof(value), NULL));
}

static void test_uint64_fields(void)
{
    uint64_t value = 0U;
    assert(solar_os_json_scan_object_uint64(
        "{\"id\":18446744073709551615}", "id", &value));
    assert(value == UINT64_MAX);
    assert(!solar_os_json_scan_object_uint64(
        "{\"id\":18446744073709551616}", "id", &value));
    assert(!solar_os_json_scan_object_uint64(
        "{\"id\":12oops}", "id", &value));
    assert(!solar_os_json_scan_object_uint64(
        "{\"id\":-1}", "id", &value));
}

static void test_escape(void)
{
    char escaped[32];
    assert(solar_os_json_escape_string(
        "a\"b\\c\n", escaped, sizeof(escaped)) == ESP_OK);
    assert(strcmp(escaped, "a\\\"b\\\\c\\n") == 0);

    char short_value[3];
    assert(solar_os_json_escape_string(
        "\"", short_value, sizeof(short_value)) == ESP_OK);
    assert(strcmp(short_value, "\\\"") == 0);
    assert(solar_os_json_escape_string(
        "\"x", short_value, sizeof(short_value)) == ESP_ERR_INVALID_SIZE);
}

int main(void)
{
    test_string_fields();
    test_invalid_strings();
    test_uint64_fields();
    test_escape();
    puts("json scan tests: ok");
    return 0;
}
