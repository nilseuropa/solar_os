#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "esp_elf.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_cc_abi.h"
#include "solar_os_native_abi.h"
#include "solar_os_native_library_abi.h"
#include "solar_os_native_libc_abi.h"
#include "solar_os_native_thread_abi.h"
#include "solar_os_native_time_abi.h"
#include "hello_library_api.h"

static const solar_os_native_library_descriptor_v1_t *compiler;
static unsigned leases, library_calls;
void *solar_os_memory_elf_alloc(size_t bytes) { return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static uint32_t add(uint32_t a, uint32_t b) { library_calls++; assert(a == 19 && b == 23); return a+b; }
static const hello_library_api_v1_t provider = {1, sizeof(provider), add};
static int acquire(const char *id, uint32_t abi, uint32_t size, uint32_t *lease, const void **table)
{
    assert(abi == 1 && !*lease && !*table);
    if (!strcmp(id, "tinycc")) { assert(size == sizeof(solar_os_cc_api_v1_t)); *table = compiler->api; }
    else { assert(!strcmp(id, "hello-library") && size == sizeof(provider)); *table = &provider; }
    *lease = ++leases; return 0;
}
static int release(uint32_t lease) { assert(lease && leases); leases--; return 0; }
static int register_library(const solar_os_native_library_descriptor_v1_t *descriptor)
{ assert(!strcmp(descriptor->id, "tinycc")); compiler = descriptor; return 0; }
static void sleep_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1); }
struct worker { StaticTask_t task_buffer; TaskHandle_t task; void (*entry)(void *); void *arg; StackType_t stack[]; };
static void worker_entry(void *value)
{ struct worker *w = value; w->entry(w->arg); vTaskSuspend(NULL); }
static int start_worker(void **out, void (*entry)(void *), void *arg, uint32_t bytes, uint32_t core)
{
    struct worker *w = heap_caps_calloc(1, sizeof(*w)+bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!w) return ENOMEM;
    w->entry = entry; w->arg = arg;
    w->task = xTaskCreateStaticPinnedToCore(worker_entry, "cc_worker", bytes, w, 1, w->stack, &w->task_buffer, core);
    assert(w->task); *out = w; return 0;
}
static int join_worker(void *value)
{ struct worker *w = value; while (eTaskGetState(w->task) != eSuspended) vTaskDelay(1); vTaskDelete(w->task); heap_caps_free(w); return 0; }
static const solar_os_native_memory_api_v1_t memory = {.abi_version=1, .struct_size=sizeof(memory), .alloc=solar_os_memory_elf_alloc, .free=heap_caps_free};
static const solar_os_native_libc_api_v1_t libc_api = {.abi_version=1, .struct_size=sizeof(libc_api), .format=vsnprintf};
static const solar_os_native_time_api_v1_t clock_api = {1, sizeof(clock_api), esp_timer_get_time, sleep_ms};
static const solar_os_native_thread_api_v1_t threads = {.abi_version=1, .struct_size=sizeof(threads), .worker_start=start_worker, .worker_join=join_worker};
static const solar_os_native_library_service_api_v1_t libraries = {.abi_version=1, .struct_size=sizeof(libraries), .acquire=acquire, .release=release};
static const void *service(const char *name, uint32_t abi, uint32_t size)
{
    assert(abi == 1);
#define OFFER(id, table) if (!strcmp(name, id) && size <= sizeof(table)) return &table
    OFFER("memory", memory); OFFER("libc", libc_api); OFFER("time", clock_api);
    OFFER("thread", threads); OFFER("library", libraries);
    return NULL;
}
static int write_text(const char *text, size_t bytes) { fwrite(text, 1, bytes, stdout); return 0; }
static const solar_os_native_host_api_v1_t host = {1, sizeof(host), "esp32s3", "runtime-emulator", write_text, service};
static const solar_os_native_library_host_api_v1_t library_host = {1, sizeof(library_host), "esp32s3", "runtime-emulator", register_library, service};
const solar_os_native_host_api_v1_t *solar_os_native_host_v1(void) { return &host; }
const solar_os_native_library_host_api_v1_t *solar_os_native_library_host_v1(void) { return &library_host; }
static esp_elf_symbol_table_t exports[] = {
    ESP_ELFSYM_EXPORT(solar_os_native_host_v1), ESP_ELFSYM_EXPORT(solar_os_native_library_host_v1),
    ESP_ELFSYM_EXPORT(memcpy), ESP_ELFSYM_EXPORT(memset), ESP_ELFSYM_EXPORT(memmove),
    ESP_ELFSYM_EXPORT(memcmp), ESP_ELFSYM_EXPORT(strlen), ESP_ELFSYM_EXPORT(setjmp), ESP_ELFSYM_EXPORT(longjmp), ESP_ELFSYM_END,
};
extern const uint8_t compiler_start[] asm("_binary_compiler_start");
extern const uint8_t client_start[] asm("_binary_client_start");
static const char program[] =
#include "library_probe.inc"
;
static const char scalar_program[] =
#include "scalar_probe.inc"
;
void app_main(void)
{
    esp_elf_t library_elf, client_elf, program_elf;
    assert(!esp_elf_register_symbol(exports));
    assert(!esp_elf_init(&library_elf) && !esp_elf_relocate(&library_elf, compiler_start));
    assert(!((int (*)(int,char **))library_elf.entry)(0, NULL));
    assert(compiler);
    assert(!esp_elf_init(&client_elf) && !esp_elf_relocate(&client_elf, client_start));
    char *argv[] = {"cc", "--self-test"};
    int code = ((int (*)(int,char **))client_elf.entry)(2, argv);
    printf("NATIVE_CLIENT_RESULT=%d\n", code); assert(!code);
    esp_elf_deinit(&client_elf);
    const solar_os_cc_api_v1_t *api = (const void *)compiler->api;
    void *output = solar_os_memory_elf_alloc(8192); assert(output);
    solar_os_cc_request_v1_t request = {.struct_size=sizeof(request), .source=program, .source_bytes=sizeof(program)-1};
    solar_os_cc_result_v1_t metrics = {.struct_size=sizeof(metrics)};
    char error[512];
    code = api->compile(&request, output, 8192, &metrics, error, sizeof(error));
    printf("NATIVE_GENERATED code=%d bytes=%u peak=%u ms=%u %s\n", code, metrics.output_bytes, metrics.peak_heap_bytes, metrics.elapsed_ms, error);
    assert(!code && metrics.output_bytes);
    assert(!esp_elf_init(&program_elf) && !esp_elf_relocate(&program_elf, output));
    assert(!((int (*)(int,char **))program_elf.entry)(0,NULL));
    assert(library_calls == 1 && !leases);
    esp_elf_deinit(&program_elf);
    request.source = scalar_program; request.source_bytes = sizeof(scalar_program)-1;
    metrics = (solar_os_cc_result_v1_t){.struct_size=sizeof(metrics)};
    code = api->compile(&request, output, 8192, &metrics, error, sizeof(error));
    printf("NATIVE_SCALAR code=%d bytes=%u %s\n", code, metrics.output_bytes, error);
    assert(!code && metrics.output_bytes);
    assert(!esp_elf_init(&program_elf) && !esp_elf_relocate(&program_elf, output));
    assert(!((int (*)(int,char **))program_elf.entry)(0,NULL));
    esp_elf_deinit(&program_elf); heap_caps_free(output);
    assert(!compiler->shutdown()); esp_elf_deinit(&library_elf);
    printf("SOLAR_CC_NATIVE_PASS: compiled C on ESP32-S3, then loaded and executed its ELF\n");
}
