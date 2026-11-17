#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "esp_elf.h"
#include "solar_os_native_abi.h"
#include "solar_os_native_library_abi.h"
#include "solar_os_native_job_abi.h"
#include "hello_library_api.h"

static unsigned acquired, called, released, written;
static unsigned char live[256];
static const solar_os_native_library_descriptor_v1_t *provider;
static const solar_os_native_job_descriptor_v1_t *worker;
static uint32_t add(uint32_t left, uint32_t right)
{
    assert(acquired == released + 1 && left == 19 && right == 23);
    called++;
    return left + right;
}
static const hello_library_api_v1_t api = {1, sizeof(api), add};
static int acquire(const char *id, uint32_t abi, uint32_t size,
                   uint32_t *lease, const void **table)
{
    assert(!strcmp(id, "hello-library") && abi == 1 && size == sizeof(api));
    assert(!*lease && !*table);
    *lease = ++acquired; assert(*lease < sizeof(live)); live[*lease] = 1;
    *table = provider ? (const void *)provider->api : &api;
    return 0;
}
static int release(uint32_t lease)
{
    assert(lease && lease < sizeof(live) && live[lease]); live[lease] = 0;
    released++;
    return 0;
}
static int retain(uint32_t lease, uint32_t *retained)
{
    assert(lease && lease < sizeof(live) && live[lease]);
    *retained = ++acquired; assert(*retained < sizeof(live)); live[*retained] = 1;
    return 0;
}
static const solar_os_native_library_service_api_v1_t library = {
    .abi_version = 1, .struct_size = sizeof(library), .acquire = acquire, .release = release, .retain = retain,
};
static const void *service(const char *name, uint32_t abi, uint32_t size)
{
    assert(!strcmp(name, "library") && abi == 1 && size == sizeof(library));
    return &library;
}
static int write_text(const char *text, size_t size)
{
    if (size == 29) {
        assert(!memcmp(text, "TinyCC: scalar checks passed\n", size));
        return 0;
    }
    assert(size == 35 && !memcmp(text, "TinyCC: native library returned 42\n", size));
    written++;
    return 0;
}
static const solar_os_native_host_api_v1_t host = {
    .abi_version = 1, .struct_size = sizeof(host), .target = "esp32s3",
    .firmware_version = "emulator-harness", .write_utf8 = write_text, .get_service = service,
};
const solar_os_native_host_api_v1_t *solar_os_native_host_v1(void) { return &host; }
static int register_provider(const solar_os_native_library_descriptor_v1_t *descriptor)
{
    assert(descriptor->abi_version == 1 && descriptor->struct_size == sizeof(*descriptor));
    assert(!strcmp(descriptor->id, "hello-library"));
    provider = descriptor; return 0;
}
static int register_worker(const solar_os_native_job_descriptor_v1_t *descriptor)
{
    assert(descriptor->abi_version == 1 && descriptor->struct_size == sizeof(*descriptor));
    assert(!strcmp(descriptor->id, "library-user"));
    worker = descriptor; return 0;
}
const solar_os_native_library_host_api_v1_t *solar_os_native_library_host_v1(void)
{
    static const solar_os_native_library_host_api_v1_t table = {
        .abi_version = 1, .struct_size = sizeof(table), .register_library = register_provider,
    };
    return &table;
}
const solar_os_native_job_host_api_v1_t *solar_os_native_job_host_v1(void)
{
    static const solar_os_native_job_host_api_v1_t table = {
        .abi_version = 1, .struct_size = sizeof(table), .register_job = register_worker, .get_service = service,
    };
    return &table;
}
static esp_elf_symbol_table_t exports[] = {
    ESP_ELFSYM_EXPORT(solar_os_native_host_v1),
    ESP_ELFSYM_EXPORT(solar_os_native_library_host_v1),
    ESP_ELFSYM_EXPORT(solar_os_native_job_host_v1), ESP_ELFSYM_END,
};
extern const uint8_t probe_start[] asm("_binary_library_probe_start");
extern const uint8_t scalar_start[] asm("_binary_scalar_probe_start");
extern const uint8_t provider_start[] asm("_binary_provider_start");
extern const uint8_t worker_start[] asm("_binary_worker_start");
void app_main(void)
{
    esp_elf_t elf;
    assert(!esp_elf_register_symbol(exports));
    assert(!esp_elf_init(&elf));
    assert(!esp_elf_relocate(&elf, scalar_start));
    int scalar_result = ((int (*)(int, char **))elf.entry)(0, NULL);
    printf("SCALAR result=%d\n", scalar_result);
    if (scalar_result) return;
    esp_elf_deinit(&elf);
    for (unsigned i = 0; i < 100; i++) {
        assert(!esp_elf_init(&elf));
        assert(!esp_elf_relocate(&elf, probe_start));
        int result = ((int (*)(int, char **))elf.entry)(0, NULL);
        if (result) { printf("PROBE FAILED result=%d\n", result); return; }
        esp_elf_deinit(&elf);
    }
    assert(acquired == 100 && called == 100 && released == 100 && written == 100);
    esp_elf_t provider_elf, worker_elf;
    assert(!esp_elf_init(&provider_elf) && !esp_elf_relocate(&provider_elf, provider_start));
    assert(!((int (*)(int, char **))provider_elf.entry)(0, NULL));
    const hello_library_api_v1_t *compiled_api = (const void *)provider->api;
    assert(compiled_api->abi_version == 1 && compiled_api->struct_size == sizeof(*compiled_api));
    assert(compiled_api->add(19, 23) == 42);
    assert(!esp_elf_init(&worker_elf) && !esp_elf_relocate(&worker_elf, worker_start));
    assert(!((int (*)(int, char **))worker_elf.entry)(0, NULL));
    assert(!worker->start(0, NULL));
    for (unsigned i = 0; i < 100; i++) assert(!worker->tick(i * 100));
    worker->stop();
    assert(acquired == released && acquired == 102);
    assert(!provider->shutdown());
    esp_elf_deinit(&worker_elf); esp_elf_deinit(&provider_elf);
    printf("SOLAR_TCC_PASS: 100 ABI cycles; generated provider and job callbacks passed\n");
}
