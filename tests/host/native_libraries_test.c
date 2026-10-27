#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include "esp_err.h"
#include "solar_os_native_elf.h"
#include "solar_os_native_library_abi.h"
#include "solar_os_native_job_abi.h"
#include "solar_os_native_driver_abi.h"

typedef void *TaskHandle_t;
static _Thread_local int task_identity;
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return &task_identity; }
typedef int solar_os_job_t;
typedef struct { bool live; } esp_elf_t;
typedef enum { SOLAR_OS_MODULE_TYPE_INVALID, SOLAR_OS_MODULE_TYPE_APP,
    SOLAR_OS_MODULE_TYPE_JOB, SOLAR_OS_MODULE_TYPE_DRIVER, SOLAR_OS_MODULE_TYPE_LIBRARY } solar_os_module_type_t;
#define SOLAR_OS_PACKAGE_SERVICE_EXPANSION 0
#define SOLAR_OS_MODULE_PACKAGE_ID_MAX 32U
#define SOLAR_OS_NATIVE_SUMMARY_MAX 96U
#define SOLAR_OS_NATIVE_RESIDENT_MAX 16U
#define SOLAR_OS_NATIVE_LIBRARY_LEASES_MAX 128U
#define SOLAR_OS_STORAGE_PATH_MAX 256U
#define SOLAR_OS_MEMORY_EXTERNAL_REQUIRED 0
#include "library_types.inc"
static native_resident_slot_t *native_resident_slots, *native_registering_slot;
static TaskHandle_t native_registering_task;
static native_library_lease_t *native_library_leases;
static uint32_t native_library_next_lease;
static size_t native_library_lease_count;
static pthread_mutex_t native_resident_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool native_running;
static atomic_int allocations, images, loads, shutdowns;
static int fail_allocation, fail_load, bad_registration, fail_shutdown;
#define portENTER_CRITICAL(lock) assert(pthread_mutex_lock(lock) == 0)
#define portEXIT_CRITICAL(lock) assert(pthread_mutex_unlock(lock) == 0)
static void vTaskDelay(int n) { (void)n; sched_yield(); }
static void native_set_detail(char *d, size_t n, const char *s) { if (d && n) snprintf(d, n, "%s", s); }
static void *solar_os_memory_calloc(size_t n, size_t size, int cls, const char *tag) {
    (void)cls; (void)tag;
    if (fail_allocation) return NULL;
    void *p = calloc(n, size); if (p) atomic_fetch_add(&allocations, 1); return p;
}
static void solar_os_memory_free(void *p) { if (p) atomic_fetch_sub(&allocations, 1); free(p); }
static esp_err_t solar_os_module_package_path(solar_os_module_type_t type, const char *id, char *path, size_t n) {
    assert(type == SOLAR_OS_MODULE_TYPE_LIBRARY);
    if (!id || strchr(id, '/') || !strcmp(id, ".") || !strcmp(id, "..")) return ESP_ERR_INVALID_ARG;
    snprintf(path, n, "modules/libraries/%s.elf", id); return ESP_OK;
}
esp_err_t solar_os_native_module_activate(solar_os_module_type_t, const char *, const char *, char *, size_t);
esp_err_t solar_os_native_module_deactivate(solar_os_module_type_t, const char *, char *, size_t);
static int native_register_library(const solar_os_native_library_descriptor_v1_t *);
static uint32_t add(uint32_t a, uint32_t b) { return a + b; }
typedef struct { uint32_t abi_version, struct_size; uint32_t (*add)(uint32_t, uint32_t); } api_t;
static const api_t api = {7, sizeof(api), add};
int solar_os_native_library_acquire(const char *, uint32_t, uint32_t, uint32_t *, const void **);
static int shutdown_library(void) {
    atomic_fetch_add(&shutdowns, 1);
    uint32_t token; const void *p;
    assert(solar_os_native_library_acquire("example", 7, sizeof(api), &token, &p) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    return fail_shutdown ? SOLAR_OS_NATIVE_ERROR_INVALID_STATE : SOLAR_OS_NATIVE_OK;
}
static esp_err_t native_load_resident(native_resident_slot_t *slot, const char *path, char *d, size_t n) {
    (void)path; (void)d; (void)n;
    if (!strcmp(slot->id, "missing")) return ESP_ERR_NOT_FOUND;
    slot->elf_initialized = slot->elf.live = true;
    atomic_fetch_add(&images, 1); atomic_fetch_add(&loads, 1);
    if (fail_load) return ESP_FAIL;
    native_registering_slot = slot; native_registering_task = xTaskGetCurrentTaskHandle();
    const solar_os_native_library_descriptor_v1_t descriptor = {
        1, sizeof(descriptor), bad_registration ? "wrong-id" : slot->id,
        "example library", (const solar_os_native_library_api_header_v1_t *)&api, shutdown_library,
    };
    int result = native_register_library(&descriptor);
    if (!result) assert(native_register_library(&descriptor) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    native_registering_slot = NULL; native_registering_task = NULL;
    return result ? ESP_ERR_INVALID_RESPONSE : ESP_OK;
}
static void esp_elf_deinit(esp_elf_t *elf) { assert(elf->live); elf->live = false; atomic_fetch_sub(&images, 1); }
static esp_err_t native_register_resident(native_resident_slot_t *slot) { assert(slot->registered); return ESP_OK; }
static esp_err_t solar_os_jobs_unregister_dynamic(const char *id, solar_os_job_t *job) { (void)id; (void)job; return ESP_OK; }
#include "library_runtime.inc"

static void acquire(uint32_t *lease, const void **p) {
    assert(solar_os_native_library_acquire("example", 7, sizeof(api), lease, p) == 0);
    assert(*lease && *p == &api);
}
static void test_lifecycle(void) {
    uint32_t a = 42, b, c; const void *p = (void *)1, *q;
    assert(solar_os_native_library_acquire("missing", 7, sizeof(api), &a, &p) == -5 && !a && !p);
    assert(!allocations && !images && !native_running);
    assert(solar_os_native_library_acquire("../example", 7, sizeof(api), &a, &p) == -2);
    assert(solar_os_native_library_acquire("example", 0, sizeof(api), &a, &p) == -2);
    assert(solar_os_native_library_acquire("example", 7, 1, &a, &p) == -2);
    assert(solar_os_native_library_retain(0, &a) == -2);
    assert(solar_os_native_library_release(0) == -2);
    fail_allocation = 1;
    assert(solar_os_native_library_acquire("example", 7, sizeof(api), &a, &p) == -4);
    fail_allocation = 0; fail_load = 1;
    assert(solar_os_native_library_acquire("example", 7, sizeof(api), &a, &p) == -1);
    fail_load = 0; bad_registration = 1;
    assert(solar_os_native_library_acquire("example", 7, sizeof(api), &a, &p) == -1);
    bad_registration = 0;
    assert(!images && !allocations && !native_resident_slots);
    acquire(&a, &p); int first_loads = loads;
    acquire(&b, &q); assert(p == q && a != b && loads == first_loads);
    assert(solar_os_native_library_retain(a, &c) == 0 && c != a && c != b);
    uint32_t bad = 99; const void *out = (void *)1;
    assert(solar_os_native_library_acquire("example", 8, sizeof(api), &bad, &out) == -6 && !bad && !out);
    assert(solar_os_native_library_acquire("example", 7, sizeof(api)+1, &bad, &out) == -6 && !bad && !out);
    assert(solar_os_native_library_unload("example") == -3 && !shutdowns && images == 1);
    assert(solar_os_native_library_release(a) == 0);
    assert(solar_os_native_library_release(a) == -5);
    assert(solar_os_native_library_retain(a, &bad) == -5 && !bad);
    assert(solar_os_native_library_release(b) == 0);
    assert(((const api_t *)p)->add(19, 23) == 42 && solar_os_native_library_unload("example") == -3);
    assert(solar_os_native_library_release(c) == 0);
    fail_shutdown = 1;
    assert(solar_os_native_library_unload("example") == -3 && images == 1);
    acquire(&b, &p); assert(solar_os_native_library_release(b) == 0);
    fail_shutdown = 0;
    assert(solar_os_native_library_unload("example") == 0);
    assert(!images && !allocations && !native_library_leases && !native_resident_slots);
    assert(solar_os_native_library_unload("example") == -5);
    acquire(&b, &p); assert(b != a && solar_os_native_library_release(a) == -5);
    uint32_t tokens[128]; tokens[0] = b;
    for (unsigned i=1; i<128; ++i) acquire(&tokens[i], &p);
    assert(solar_os_native_library_retain(b, &bad) == -4 && !bad);
    assert(solar_os_native_library_acquire("example", 7, sizeof(api), &bad, &out) == -4 && !bad && !out);
    for (unsigned i=0; i<128; ++i) assert(solar_os_native_library_release(tokens[i]) == 0);
    uint32_t old_next = native_library_next_lease;
    native_library_next_lease = UINT32_MAX;
    assert(solar_os_native_library_acquire("example", 7, sizeof(api), &bad, &out) == -4 && !bad && !out);
    native_library_next_lease = old_next;
    assert(solar_os_native_library_unload("example") == 0);
    assert(!images && !allocations && !native_library_lease_count);
}
static void test_registration(void) {
    native_resident_slot_t slot = {.type = SOLAR_OS_MODULE_TYPE_LIBRARY, .id = "example"};
    solar_os_native_library_descriptor_v1_t descriptor = {
        1, sizeof(descriptor), "example", "example library",
        (const solar_os_native_library_api_header_v1_t *)&api, shutdown_library,
    };
    assert(native_register_library(&descriptor) == -2);
    native_registering_slot = &slot; native_registering_task = NULL;
    assert(native_register_library(&descriptor) == -2);
    native_registering_task = xTaskGetCurrentTaskHandle();
    descriptor.abi_version = 2; assert(native_register_library(&descriptor) == -2);
    descriptor.abi_version = 1; descriptor.struct_size--;
    assert(native_register_library(&descriptor) == -2); descriptor.struct_size++;
    descriptor.shutdown = NULL; assert(native_register_library(&descriptor) == -2);
    descriptor.shutdown = shutdown_library;
    solar_os_native_library_api_header_v1_t bad = {0, sizeof(bad)};
    descriptor.api = &bad; assert(native_register_library(&descriptor) == -2);
    bad.abi_version = 1; bad.struct_size--;
    assert(native_register_library(&descriptor) == -2);
    native_registering_slot = NULL; native_registering_task = NULL;
}
static void *reader(void *unused) {
    (void)unused;
    for (unsigned i=0; i<2000; ++i) {
        uint32_t token; const void *p;
        int r = solar_os_native_library_acquire("example", 7, sizeof(api), &token, &p);
        if (r == -3 || r == -5) { assert(!token && !p); sched_yield(); continue; }
        assert(r == 0 && ((const api_t *)p)->add(i, 1) == i+1);
        uint32_t retained; assert(solar_os_native_library_retain(token, &retained) == 0);
        assert(solar_os_native_library_release(token) == 0);
        assert(((const api_t *)p)->add(i, 2) == i+2);
        assert(solar_os_native_library_release(retained) == 0);
    }
    return NULL;
}
static void *unloader(void *unused) {
    (void)unused;
    for (unsigned i=0; i<3000; ++i) {
        int r = solar_os_native_library_unload("example");
        assert(r == 0 || r == -3 || r == -5); sched_yield();
    }
    return NULL;
}
int main(void) {
    test_registration();
    test_lifecycle();
    uint32_t lease; const void *table;
    assert(solar_os_native_library_acquire("example", 7, sizeof(api), &lease, &table) == 0);
    native_resident_slot_t retained_app = {.type = SOLAR_OS_MODULE_TYPE_APP, .id = "retained"};
    assert(native_slot_add(&retained_app));
    assert(native_reclaim_unused_libraries() == 0); /* Active client protects its provider. */
    assert(solar_os_native_library_release(lease) == 0);
    fail_shutdown = 1;
    assert(native_reclaim_unused_libraries() == 0); /* Failed shutdown keeps code resident. */
    fail_shutdown = 0;
    assert(native_reclaim_unused_libraries() == 1);
    assert(solar_os_native_module_active(SOLAR_OS_MODULE_TYPE_APP, "retained"));
    native_slot_remove(&retained_app);
    assert(!allocations && !images);
    pthread_t threads[4];
    for (unsigned i=0;i<3;++i) assert(!pthread_create(&threads[i], NULL, reader, NULL));
    assert(!pthread_create(&threads[3], NULL, unloader, NULL));
    for (unsigned i=0;i<4;++i) assert(!pthread_join(threads[i], NULL));
    int r = solar_os_native_library_unload("example"); assert(r == 0 || r == -5);
    assert(!allocations && !images && !native_library_leases && !native_library_lease_count);
    puts("Native libraries: lifecycle, leases, safe reclamation, rollback and concurrent unload passed");
    return 0;
}
