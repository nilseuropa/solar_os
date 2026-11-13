#include <assert.h>
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
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
static solar_os_native_libc_api_v1_t libc_api = {.abi_version=1, .struct_size=sizeof(libc_api), .format=vsnprintf,
    .parse_float=strtof,.parse_double=strtod,.scale_double=ldexp,.floor_value=floor,.sqrt_float=sqrtf};
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
static int header_reader(void *user, const char *path, void *buffer, uint32_t capacity, uint32_t *bytes)
{
    assert(user == &library_calls);
    const char *text = !strcmp(path, "/project/include/outer.h") ? "#include \"detail/answer.h\"\n" :
        !strcmp(path, "/project/include/detail/answer.h") ? "#define ANSWER 42\n" : NULL;
    if (!text) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    *bytes = strlen(text);
    if (buffer) { assert(capacity == *bytes); memcpy(buffer, text, *bytes); }
    return 0;
}
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
    int scalar_result=((int (*)(int,char **))program_elf.entry)(0,NULL);
    printf("NATIVE_SCALAR_RESULT=%d\n",scalar_result); assert(!scalar_result);
    esp_elf_deinit(&program_elf);
    /* The old request prefix remains compatible with the extended library. */
    request = (solar_os_cc_request_v1_t){.struct_size=SOLAR_OS_CC_REQUEST_BASE_SIZE,
        .source="int main(void) { return 42; }", .source_bytes=28};
    request.source_bytes = strlen(request.source);
    metrics = (solar_os_cc_result_v1_t){.struct_size=sizeof(metrics)};
    assert(!api->compile(&request, output, 8192, &metrics, error, sizeof(error)));
    assert(!esp_elf_init(&program_elf) && !esp_elf_relocate(&program_elf, output));
    assert(((int (*)(int,char **))program_elf.entry)(0,NULL) == 42);
    esp_elf_deinit(&program_elf);
    const char *paths[] = {"/project/include"};
    request = (solar_os_cc_request_v1_t){.struct_size=sizeof(request),
        .source="#include <outer.h>\nint main(void) { return ANSWER; }",
        .source_name="/project/src/input.c", .include_paths=paths, .include_count=1,
        .read_header=header_reader, .header_user=&library_calls};
    request.source_bytes = strlen(request.source);
    metrics = (solar_os_cc_result_v1_t){.struct_size=sizeof(metrics)};
    assert(!api->compile(&request, output, 8192, &metrics, error, sizeof(error)));
    assert(!esp_elf_init(&program_elf) && !esp_elf_relocate(&program_elf, output));
    assert(((int (*)(int,char **))program_elf.entry)(0,NULL) == 42);
    esp_elf_deinit(&program_elf);
    request.source = "extern int unknown(void); int main(void) { return unknown(); }";
    request.source_bytes = strlen(request.source);
    metrics = (solar_os_cc_result_v1_t){.struct_size=sizeof(metrics)};
    assert(api->compile(&request, output, 8192, &metrics, error, sizeof(error)) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    assert(!metrics.output_bytes && strstr(error, "unsupported native import"));
    /* Old firmware still compiles integers, and reports a missing optional
     * parser before dereferencing the appended libc methods. */
    request=(solar_os_cc_request_v1_t){.struct_size=sizeof(request),
        .source="int main(void){return 6*7-42;}"};
    request.source_bytes=strlen(request.source);
    libc_api.struct_size=offsetof(solar_os_native_libc_api_v1_t,parse_double);
    metrics=(solar_os_cc_result_v1_t){.struct_size=sizeof(metrics)};
    assert(!api->compile(&request,output,8192,&metrics,error,sizeof(error)));
    assert(!esp_elf_init(&program_elf) && !esp_elf_relocate(&program_elf,output));
    assert(!((int (*)(int,char **))program_elf.entry)(0,NULL));esp_elf_deinit(&program_elf);
    request.source="int main(void){union {double value;unsigned words[2];} x={1.25};return x.words[0] || x.words[1]!=0x3ff40000U;}";
    request.source_bytes=strlen(request.source);
    metrics=(solar_os_cc_result_v1_t){.struct_size=sizeof(metrics)};
    assert(api->compile(&request,output,8192,&metrics,error,sizeof(error))==SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    assert(!metrics.output_bytes && strstr(error,"parse_double"));
    libc_api.struct_size=sizeof(libc_api);libc_api.parse_double=NULL;
    metrics=(solar_os_cc_result_v1_t){.struct_size=sizeof(metrics)};
    assert(api->compile(&request,output,8192,&metrics,error,sizeof(error))==SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    assert(!metrics.output_bytes && strstr(error,"parse_double"));libc_api.parse_double=strtod;
    request.source="int main(void){return 0x1.4p0!=1.25;}";request.source_bytes=strlen(request.source);
    libc_api.struct_size=offsetof(solar_os_native_libc_api_v1_t,scale_double);
    metrics=(solar_os_cc_result_v1_t){.struct_size=sizeof(metrics)};
    assert(api->compile(&request,output,8192,&metrics,error,sizeof(error))==SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    assert(!metrics.output_bytes && strstr(error,"scale_double"));
    libc_api.struct_size=sizeof(libc_api);libc_api.scale_double=NULL;
    metrics=(solar_os_cc_result_v1_t){.struct_size=sizeof(metrics)};
    assert(api->compile(&request,output,8192,&metrics,error,sizeof(error))==SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    assert(!metrics.output_bytes && strstr(error,"scale_double"));libc_api.scale_double=ldexp;
    libc_api.struct_size=sizeof(libc_api);
    metrics=(solar_os_cc_result_v1_t){.struct_size=sizeof(metrics)};
    request.source="int main(void){union {double value;unsigned words[2];} x={0x1.4p0};return x.words[0] || x.words[1]!=0x3ff40000U;}";
    request.source_bytes=strlen(request.source);
    assert(!api->compile(&request,output,8192,&metrics,error,sizeof(error)));
    assert(!esp_elf_init(&program_elf) && !esp_elf_relocate(&program_elf,output));
    assert(!((int (*)(int,char **))program_elf.entry)(0,NULL));esp_elf_deinit(&program_elf);
    printf("SOLAR_CC_LIBC_EXTENSION_PASS: missing-parser rejection and recovery\n");
    heap_caps_free(output);
#ifdef SOLAR_CC_DIFFERENTIAL
    extern int solar_cc_differential(const solar_os_cc_api_v1_t *);
    assert(!solar_cc_differential(api));
#endif
    assert(!compiler->shutdown()); esp_elf_deinit(&library_elf);
    printf("SOLAR_CC_NATIVE_PASS: compiled C on ESP32-S3, then loaded and executed its ELF\n");
    printf("SOLAR_CC_FILE_ADAPTER_PASS: legacy requests, standalone main, nested disk headers and imports\n");
#ifdef SOLAR_CC_DIFFERENTIAL
    printf("SOLAR_CC_DIFFERENTIAL_PASS: GCC O0/O2, host TinyCC and on-device TinyCC agree\n");
#endif
}
