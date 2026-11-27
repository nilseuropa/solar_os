#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "solar_os_native_elf.h"
#include "solar_os_native_library_abi.h"
static const solar_os_native_library_service_api_v1_t native_library_api = { .abi_version=1, .struct_size=sizeof(native_library_api) };
#include "solar_os_native_time_abi.h"
static const solar_os_native_time_api_v1_t native_time_api = { .abi_version=1, .struct_size=sizeof(native_time_api) };
#include "solar_os_native_thread_abi.h"
static const solar_os_native_thread_api_v1_t native_thread_api = { .abi_version=1, .struct_size=sizeof(native_thread_api) };
#include "solar_os_native_libc_abi.h"
static const solar_os_native_libc_api_v1_t native_libc_api = { .abi_version=1, .struct_size=sizeof(native_libc_api) };
#include "solar_os_native_registry.h"
static uint32_t native_system_service_count(void);
static int native_system_service_info(uint32_t, solar_os_native_system_service_v1_t *);
static const solar_os_native_system_api_v1_t native_system_api = {
    .abi_version = 1, .struct_size = sizeof(native_system_api),
    .service_count = native_system_service_count, .service_info = native_system_service_info,
};
typedef void *TaskHandle_t;
static _Thread_local TaskHandle_t current_task;
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
static bool native_reclaimer_registered;
static const int native_reclaimer;
static bool solar_os_memory_register_reclaimer(const int *p) { assert(p == &native_reclaimer); return true; }
static void solar_os_native_operation_end(void);
static _Thread_local bool native_operation_locked;
struct native_run_scope;
static _Thread_local struct native_run_scope *native_run_scope_top;
static struct { uint32_t abi, size; } outer={1,8}, inner={1,8}, foreign={1,8};
static int active_images, cleanup_count, allocation_count;
static int fail_init, fail_relocate;
static bool foreground_image;
static void native_set_detail(char *s, size_t n, const char *text) { if (s && n) snprintf(s, n, "%s", text); }
static esp_err_t solar_os_storage_stat(const char *p, solar_os_storage_metadata_t *m) {
    (void)p; *m = (solar_os_storage_metadata_t){1, 64}; return ESP_OK;
}
static void *solar_os_memory_alloc(size_t n, int cls, const char *tag) {
    assert(native_operation_locked && atomic_load(&native_running));
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
    assert(native_operation_locked && atomic_load(&native_running));
    (void)elf; (void)data; return fail_relocate;
}
static int esp_elf_request(esp_elf_t *, int, int, char **);
static void esp_elf_deinit(esp_elf_t *elf) {
    assert(elf->live); assert(cleanup_count >= 1); elf->live = false; active_images--;
}
#include "native_scope_providers.inc"
static const struct { uint32_t abi, size; } motion_service_marker = {1, 8};
static const void *solar_os_native_motion_get_service(const char *s, uint32_t a, uint32_t n) {
    return s && !strcmp(s, "motion") && a == 1 && n <= sizeof(motion_service_marker) ? &motion_service_marker : NULL;
}
#include "native_run.inc"

static esp_err_t write_output(const char *text, size_t n, void *user) {
    assert(user == current_task); assert(n == 1U && text[0] == 'x'); return ESP_OK;
}
static const void *service(const char *s, uint32_t abi, uint32_t size, void *user) {
    if (strcmp(s, "foreground")) return NULL;
    assert(abi == 1U && size == 8U);
    assert(user == current_task);
    return user;
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
    assert(native_job_get_service("motion", 1U, 4U) == &motion_service_marker);
    for (uint32_t i=0; i<native_system_service_count(); ++i) {
        solar_os_native_system_service_v1_t info;
        assert(!native_system_service_info(i,&info));
        if (!strcmp(info.name,"foreground")) {
            assert(info.struct_size==8);
            assert(info.flags==(SOLAR_OS_NATIVE_SYSTEM_SCOPED|SOLAR_OS_NATIVE_SYSTEM_AVAILABLE));
        }
    }
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
static unsigned builtin_cleanups;
static void builtin_cleanup(void *user) {
    assert(user == current_task && solar_os_native_run_service_user() == user);
    builtin_cleanups++;
}
static int builtin_entry(int argc, char **argv) {
    assert(argc == 1 && argv && !active_images && !allocation_count);
    assert(native_job_get_service("foreground", 1U, 8U) == current_task);
    assert(native_write_utf8("x", 1) == ESP_OK);
    return 23;
}
int main(void) {
    current_task = &outer;
    assert(native_system_service_count() >= 20);
    assert(native_job_get_service("system", 2, 8) == NULL);
    assert(native_job_get_service(NULL, 1, 8) == NULL);
    assert(native_job_get_service("system", 1, sizeof(native_system_api)+1) == NULL);
    solar_os_native_system_service_v1_t info;
    assert(native_system_service_info(0, NULL) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    memset(&info, 0xa5, sizeof(info));
    assert(native_system_service_info(UINT32_MAX, &info) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(info.name[0] == 0 && info.struct_size == 0 && info.flags == 0);
    unsigned available=0, scoped=0;
    for (uint32_t i=0; i<native_system_service_count(); ++i) {
        assert(!native_system_service_info(i, &info));
        assert(info.name[0] && info.abi_version == 1);
        if (info.flags & SOLAR_OS_NATIVE_SYSTEM_AVAILABLE) {
            available++;
            assert(info.struct_size >= 8);
        } else assert(info.struct_size == 0);
        if (info.flags & SOLAR_OS_NATIVE_SYSTEM_SCOPED) {
            scoped++;
            assert(!(info.flags & SOLAR_OS_NATIVE_SYSTEM_AVAILABLE));
        }
    }
    assert(available == 6 && scoped >= 2);
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
    fail_relocate = -ENOMEM;
    char detail[160];
    assert(solar_os_native_run("no-memory", 1, argv, &opts, NULL, detail, sizeof(detail)) == ESP_ERR_NO_MEM);
    assert(strstr(detail, "not enough memory"));
    fail_relocate = 0; foreground_image = true;
    assert(solar_os_native_run("foreground", 1, argv, &opts, NULL, NULL, 0) == ESP_ERR_INVALID_STATE);
    assert(!active_images && !allocation_count && !native_run_scope_top && !atomic_load(&native_running));
    opts.cleanup = builtin_cleanup;
    int status = -1;
    assert(solar_os_native_run_builtin(NULL, 1, argv, &opts, &status) == ESP_ERR_INVALID_ARG);
    assert(solar_os_native_run_builtin(builtin_entry, 1, argv, &opts, &status) == ESP_OK);
    assert(status == 23 && builtin_cleanups == 1 && !native_run_scope_top && !active_images);
    puts("Native execution scopes preserve task ownership and clean up before ELF unload");
    return 0;
}
