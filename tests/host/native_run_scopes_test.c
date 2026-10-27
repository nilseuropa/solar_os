#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "solar_os_native_elf.h"
#include "solar_os_native_library_abi.h"
static const solar_os_native_library_service_api_v1_t native_library_api = {0};
#include "solar_os_native_time_abi.h"
static const solar_os_native_time_api_v1_t native_time_api = {0};
#include "solar_os_native_thread_abi.h"
static const solar_os_native_thread_api_v1_t native_thread_api = {0};
#include "solar_os_native_libc_abi.h"
static const solar_os_native_libc_api_v1_t native_libc_api = {0};
typedef void *TaskHandle_t;
static _Thread_local TaskHandle_t current_task;
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current_task; }
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define SOLAR_OS_PACKAGE_SERVICE_BLE 0
#define SOLAR_OS_STORAGE_ENTRY_FILE 1
#define SOLAR_OS_MEMORY_EXTERNAL_REQUIRED 0
#define SOLAR_OS_NATIVE_ELF_MAX_BYTES (2U * 1024U * 1024U)
typedef esp_err_t (*solar_os_native_write_fn)(const char *, size_t, void *);
typedef struct {
    solar_os_native_write_fn write;
    void *write_user;
    const void *(*get_service)(const char *, uint32_t, uint32_t, void *);
    void *service_user;
    void (*cleanup)(void *);
    const solar_os_native_app_manifest_v2_t *expected_manifest;
} solar_os_native_run_options_t;
typedef struct { solar_os_native_elf_info_t elf; } solar_os_native_run_result_t;
typedef struct { int type; size_t size_bytes; } solar_os_storage_metadata_t;
typedef struct { bool live; } esp_elf_t;
static atomic_bool native_running;
struct native_run_scope;
static _Thread_local struct native_run_scope *native_run_scope_top;
static int outer, inner, foreign, active_images, cleanup_count, allocation_count;
static int fail_init, fail_relocate;
static bool foreground_image;
static void native_set_detail(char *s, size_t n, const char *text) { if (s && n) snprintf(s, n, "%s", text); }
static esp_err_t solar_os_storage_stat(const char *p, solar_os_storage_metadata_t *m) {
    (void)p; *m = (solar_os_storage_metadata_t){1, 64}; return ESP_OK;
}
static void *solar_os_memory_alloc(size_t n, int cls, const char *tag) {
    (void)cls; (void)tag; allocation_count++; return malloc(n);
}
static void solar_os_memory_free(void *p) { if (p) allocation_count--; free(p); }
static esp_err_t solar_os_storage_read_file(const char *p, void *b, size_t n, size_t *r) {
    (void)p; memset(b, 0, n); *r = n; return ESP_OK;
}
esp_err_t solar_os_native_elf_validate(const uint8_t *b, size_t n, uint16_t machine,
    solar_os_native_elf_info_t *info, char *d, size_t dl) {
    (void)b; (void)machine; (void)d; (void)dl;
    *info = (solar_os_native_elf_info_t){.file_size = n, .foreground = foreground_image}; return ESP_OK;
}
static esp_err_t native_register_symbols(char *d, size_t n) { (void)d; (void)n; return ESP_OK; }
static int esp_elf_init(esp_elf_t *elf) {
    if (fail_init) return -1;
    elf->live = true; active_images++; return 0;
}
static int esp_elf_relocate(esp_elf_t *elf, const void *data) {
    (void)elf; (void)data; return fail_relocate ? -1 : 0;
}
static int esp_elf_request(esp_elf_t *, int, int, char **);
static void esp_elf_deinit(esp_elf_t *elf) {
    assert(elf->live); assert(cleanup_count >= 1); elf->live = false; active_images--;
}
static const void *solar_os_native_media_get_service(const char *s, uint32_t a, uint32_t n) {
    (void)s; (void)a; (void)n; return NULL;
}
#include "native_run.inc"

static esp_err_t write_output(const char *text, size_t n, void *user) {
    assert(user == current_task); assert(n == 1U && text[0] == 'x'); return ESP_OK;
}
static const void *service(const char *s, uint32_t abi, uint32_t size, void *user) {
    assert(!strcmp(s, "foreground") && abi == 1U && size == 8U);
    assert(user == current_task); return user;
}
static void cleanup(void *user) {
    assert(user == current_task); assert(active_images > 0);
    assert(solar_os_native_run_service_user() == user); cleanup_count++;
}
static void *interleaved(void *arg) {
    char **argv = arg;
    current_task = &foreign;
    assert(solar_os_native_run_service_user() == NULL);
    assert(native_write_utf8("x", 1U) == ESP_ERR_INVALID_ARG);
    current_task = &inner;
    const solar_os_native_run_options_t opts = {write_output, &inner, service, &inner, cleanup, NULL};
    assert(solar_os_native_run("inner", 1, argv, &opts, NULL, NULL, 0) == ESP_OK);
    assert(active_images == 1 && solar_os_native_run_service_user() == NULL);
    return NULL;
}
static int esp_elf_request(esp_elf_t *elf, int unused, int argc, char **argv) {
    static _Thread_local unsigned depth;
    ++depth;
    (void)unused; assert(elf->live && argc == 1 && argv);
    assert(!atomic_load(&native_running) && allocation_count == 0);
    assert(native_write_utf8("x", 1U) == ESP_OK);
    assert(native_job_get_service("foreground", 1U, 8U) == current_task);
    if (current_task == &outer && depth == 1) {
        native_run_scope_t *saved = native_run_scope_current();
        pthread_t thread;
        assert(!pthread_create(&thread, NULL, interleaved, argv));
        assert(!pthread_join(thread, NULL));
        assert(native_run_scope_current() == saved);
        const solar_os_native_run_options_t opts = {write_output, &outer, service, &outer, cleanup, NULL};
        assert(solar_os_native_run("nested", 1, argv, &opts, NULL, NULL, 0) == ESP_OK);
        assert(native_run_scope_current() == saved);
        assert(solar_os_native_run_service_user() == &outer);
        assert(native_write_utf8("x", 1U) == ESP_OK);
    }
    --depth;
    return 0;
}
int main(void) {
    current_task = &outer;
    char *argv[] = {"app"};
    solar_os_native_run_options_t opts = {write_output, &outer, service, &outer, cleanup, NULL};
    assert(solar_os_native_run("outer", 1, argv, &opts, NULL, NULL, 0) == ESP_OK);
    assert(cleanup_count == 3 && !active_images && !allocation_count && !native_run_scope_top);
    assert(!atomic_load(&native_running));
    opts.cleanup = NULL;
    fail_init = 1;
    assert(solar_os_native_run("bad", 1, argv, &opts, NULL, NULL, 0) == ESP_FAIL);
    fail_init = 0; fail_relocate = 1;
    assert(solar_os_native_run("bad", 1, argv, &opts, NULL, NULL, 0) == ESP_ERR_INVALID_RESPONSE);
    fail_relocate = 0; foreground_image = true;
    assert(solar_os_native_run("foreground", 1, argv, &opts, NULL, NULL, 0) == ESP_ERR_INVALID_STATE);
    assert(!active_images && !allocation_count && !native_run_scope_top && !atomic_load(&native_running));
    puts("Native execution scopes preserve task ownership and clean up before ELF unload");
    return 0;
}
