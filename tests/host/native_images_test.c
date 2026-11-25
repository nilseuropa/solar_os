#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_native.h"
#include "solar_os_memory.h"
#include "esp_elf.h"

static unsigned allocations, allocation_attempts, allocation_failure, initializations, deinitializations;
static int stat_failure, read_failure, validation_failure, symbols_failure, init_failure, relocation_failure;
static bool short_read;
static uint64_t file_size = 64;

void *solar_os_memory_alloc(size_t bytes, solar_os_memory_class_t memory_class, const char *tag)
{
    assert(memory_class == SOLAR_OS_MEMORY_EXTERNAL_REQUIRED && tag);
    if (++allocation_attempts == allocation_failure) return NULL;
    void *result = malloc(bytes);
    if (result) ++allocations;
    return result;
}
void *solar_os_memory_calloc(size_t count, size_t bytes, solar_os_memory_class_t memory_class, const char *tag)
{
    void *result = solar_os_memory_alloc(count * bytes, memory_class, tag);
    if (result) memset(result, 0, count * bytes);
    return result;
}
void solar_os_memory_free(void *memory)
{
    if (memory) { assert(allocations); --allocations; }
    free(memory);
}
esp_err_t solar_os_storage_stat(const char *path, solar_os_storage_metadata_t *metadata)
{
    assert(path && !strcmp(path, "test.elf"));
    *metadata = (solar_os_storage_metadata_t){.type = SOLAR_OS_STORAGE_ENTRY_FILE, .size_bytes = file_size};
    return stat_failure;
}
esp_err_t solar_os_storage_read_file(const char *path, void *data, size_t capacity, size_t *size)
{
    assert(path && data && capacity == file_size);
    *size = capacity - short_read;
    memset(data, 0, *size);
    return read_failure;
}
esp_err_t solar_os_native_elf_validate(const uint8_t *data, size_t length, uint16_t machine,
    solar_os_native_elf_info_t *info, char *detail, size_t detail_len)
{
    (void)detail; (void)detail_len;
    assert(data && length == file_size && machine == SOLAR_OS_NATIVE_ELF_MACHINE_XTENSA);
    memset(info, 0, sizeof(*info));
    return validation_failure;
}
const solar_os_native_host_api_v1_t *solar_os_native_host_v1(void) { return NULL; }
const solar_os_native_job_host_api_v1_t *solar_os_native_job_host_v1(void) { return NULL; }
const solar_os_native_driver_host_api_v1_t *solar_os_native_driver_host_v1(void) { return NULL; }
const solar_os_native_library_host_api_v1_t *solar_os_native_library_host_v1(void) { return NULL; }
int esp_elf_register_symbol(const struct esp_elfsym *symbols) { assert(symbols); return symbols_failure; }
int esp_elf_init(esp_elf_t *elf)
{
    assert(elf && !elf->initialized);
    if (init_failure) return init_failure;
    elf->initialized = true; ++initializations;
    return 0;
}
int esp_elf_relocate(esp_elf_t *elf, const void *data)
{ assert(elf && elf->initialized && data); return relocation_failure; }
int esp_elf_request(esp_elf_t *elf, int unused, int argc, char **argv)
{ assert(elf && elf->initialized && !unused && argc == 1 && argv && !strcmp(argv[0], "test")); return 19; }
void esp_elf_deinit(esp_elf_t *elf)
{ assert(elf && elf->initialized); elf->initialized = false; ++deinitializations; }

#include "../../src/services/solar_os_native.c"

static void fails(esp_err_t expected)
{
    void *image = (void *)1;
    assert(solar_os_native_image_load("test.elf", &image, NULL, 0) == expected);
    assert(!image && !allocations && initializations == deinitializations);
}
int main(void)
{
    stat_failure = ESP_FAIL; fails(ESP_ERR_NOT_FOUND); stat_failure = 0;
    file_size = 51; fails(ESP_ERR_INVALID_SIZE);
    file_size = SOLAR_OS_NATIVE_ELF_MAX_BYTES + 1; fails(ESP_ERR_INVALID_SIZE); file_size = 64;
    for (unsigned index = 1; index <= 2; ++index) {
        allocation_attempts = 0; allocation_failure = index;
        fails(ESP_ERR_NO_MEM);
    }
    allocation_failure = 0;
    read_failure = ESP_FAIL; fails(ESP_FAIL); read_failure = 0;
    short_read = true; fails(ESP_FAIL); short_read = false;
    validation_failure = ESP_ERR_INVALID_RESPONSE; fails(ESP_ERR_INVALID_RESPONSE); validation_failure = 0;
    atomic_store(&native_symbols_registered, false);
    symbols_failure = -1; fails(ESP_FAIL); symbols_failure = 0;
    init_failure = -1; fails(ESP_FAIL); init_failure = 0;
    relocation_failure = -ENOMEM; fails(ESP_ERR_NO_MEM);
    relocation_failure = -EINVAL; fails(ESP_ERR_INVALID_RESPONSE); relocation_failure = 0;
    void *image;
    assert(!solar_os_native_image_load("test.elf", &image, NULL, 0) && image && allocations == 1);
    char *argv[] = {"test", NULL};
    assert(solar_os_native_image_request(image, 1, argv) == 19);
    solar_os_native_image_close(image);
    solar_os_native_image_close(NULL);
    assert(!allocations && initializations == deinitializations);
    return 0;
}
