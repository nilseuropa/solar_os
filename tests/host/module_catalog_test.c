#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_module_packages.h"
#include "solar_os_native_abi.h"
#include "solar_os_native_job_abi.h"
#include "solar_os_native_driver_abi.h"
#include "solar_os_native_library_abi.h"
#include "solar_os_memory.h"
#include "solar_os_json.h"
#include "freertos/FreeRTOS.h"

#define SOLAR_OS_VERSION "test"
#define CONFIG_IDF_TARGET "host"
#define SOLAR_OS_NATIVE_ELF_MAX_BYTES (2U * 1024U * 1024U)
#define MODULE_CATALOG_SCHEMA_VERSION 3U
static unsigned allocations;
static size_t entries_bytes;
static const char *fail_tag;
static portMUX_TYPE module_catalog_cache_lock = portMUX_INITIALIZER_UNLOCKED;
static struct { size_t count; char ids[SOLAR_OS_MODULE_PACKAGE_COUNT_MAX][32]; } module_catalog_id_cache;
void *solar_os_memory_alloc(size_t bytes, solar_os_memory_class_t memory_class, const char *tag)
{
    (void)memory_class;
    if (fail_tag && !strcmp(fail_tag, tag)) return NULL;
    void *result = malloc(bytes);
    if (result) ++allocations;
    return result;
}
void *solar_os_memory_calloc(size_t count, size_t bytes, solar_os_memory_class_t memory_class, const char *tag)
{
    if (!strcmp(tag, "modules.entries")) entries_bytes = count * bytes;
    void *result = solar_os_memory_alloc(count * bytes, memory_class, tag);
    if (result) memset(result, 0, count * bytes);
    return result;
}
void solar_os_memory_free(void *memory)
{ if (memory) { assert(allocations); --allocations; } free(memory); }
size_t strlcpy(char *out, const char *text, size_t capacity)
{ size_t size = strlen(text); if (capacity) snprintf(out, capacity, "%s", text); return size; }
const solar_os_component_t *solar_os_component_find(solar_os_module_type_t type, const char *id)
{ (void)type; (void)id; return NULL; }
static bool solar_os_native_service_available(const char *name, uint32_t abi, uint32_t size)
{ return name && abi == 1 && size == 8; }
static bool module_capability_available(const char *name) { (void)name; return true; }
static bool solar_os_crypto_sha256_hex_is_valid(const char *value)
{ return strlen(value) == 64 && strspn(value, "0123456789abcdef") == 64; }
static esp_err_t module_verify_catalog_signature(const void *body, size_t length, const char *signature)
{ assert(body && length && !strcmp(signature, "verified")); return ESP_OK; }
esp_err_t solar_os_module_package_path(solar_os_module_type_t type, const char *id, char *out, size_t capacity)
{ (void)type; return snprintf(out, capacity, "/modules/%s.elf", id) < (int)capacity ? ESP_OK : ESP_ERR_INVALID_SIZE; }
esp_err_t solar_os_storage_exists(const char *path, bool *exists)
{ assert(path); *exists = false; return ESP_OK; }
/* BASICS */
#include "solar_os_module_requirements.inc"
/* PARSER */

int main(int argc, char **argv)
{
    assert(argc == 4);
    FILE *file = fopen(argv[1], "rb"); assert(file);
    assert(!fseek(file, 0, SEEK_END)); long size = ftell(file); assert(size > 0);
    rewind(file);
    char *body = malloc(size); assert(body && fread(body, 1, size, file) == (size_t)size && !fclose(file));
    solar_os_module_catalog_t *catalog = (void *)1;
    const esp_err_t expected = atoi(argv[2]);
    assert(module_catalog_decode(body, size, "verified", "https://example.invalid/test", &catalog, NULL, 0) == expected);
    if (expected == ESP_OK) {
        const size_t count = strtoul(argv[3], NULL, 10);
        assert(catalog && catalog->count == count && module_catalog_id_cache.count == count);
        assert(allocations == (count ? 2U : 1U));
        assert(entries_bytes == count * sizeof(*catalog->packages));
        if (count) assert(!strcmp(catalog->packages[count - 1].version, "1.0.0"));
        solar_os_module_catalog_free(catalog);
        assert(!allocations);
        for (unsigned index = 0; index < (count ? 2U : 1U); ++index) {
            fail_tag = index ? "modules.entries" : "modules.catalog";
            assert(module_catalog_decode(body, size, "verified", "https://example.invalid/test", &catalog, NULL, 0) == ESP_ERR_NO_MEM);
            assert(!catalog && !allocations && module_catalog_id_cache.count == count);
        }
    } else assert(!catalog && !allocations && !module_catalog_id_cache.count);
    solar_os_module_catalog_free(NULL);
    free(body);
    return 0;
}
