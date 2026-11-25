#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include "solar_os_native_runtime.h"
#include "solar_os_native_image.h"
#include "solar_os_jobs.h"
#include "solar_os_memory.h"
_Thread_local int solar_test_task_identity;
static atomic_int allocations, images, loads, shutdowns;
static int fail_allocation, fail_load, bad_registration, fail_shutdown, fail_reclaimer;
static int builtin_loads, builtin_shutdowns, fail_builtin;
static int builtin_entry(int, char **);
static const solar_os_component_t builtin = {
    .type = SOLAR_OS_MODULE_TYPE_LIBRARY, .id = "tinycc", .name = "Compiler",
    .version = "1.0.0", .lifecycle_abi = 1, .entry = builtin_entry,
};
const solar_os_component_t *solar_os_component_find(solar_os_module_type_t type, const char *id)
{ return id && !strcmp(id, builtin.id) && (!type || type == builtin.type) ? &builtin : NULL; }
bool solar_os_memory_register_reclaimer(solar_os_memory_reclaimer_t *reclaimer)
{ assert(reclaimer && reclaimer->reclaim); return !fail_reclaimer; }
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t cls, const char *tag) {
    (void)cls; (void)tag;
    if (fail_allocation) return NULL;
    void *p = calloc(n, size); if (p) atomic_fetch_add(&allocations, 1); return p;
}
void solar_os_memory_free(void *p) { if (p) atomic_fetch_sub(&allocations, 1); free(p); }
esp_err_t solar_os_module_package_path(solar_os_module_type_t type, const char *id, char *path, size_t n) {
    assert(type == SOLAR_OS_MODULE_TYPE_LIBRARY);
    if (!id || strchr(id, '/') || !strcmp(id, ".") || !strcmp(id, "..")) return ESP_ERR_INVALID_ARG;
    snprintf(path, n, "%s", id); return ESP_OK;
}
static uint32_t add(uint32_t a, uint32_t b) { return a + b; }
typedef struct { uint32_t abi_version, struct_size; uint32_t (*add)(uint32_t, uint32_t); } api_t;
static const api_t api = {7, sizeof(api), add};
static int shutdown_library(void) {
    atomic_fetch_add(&shutdowns, 1);
    uint32_t token; const void *p;
    assert(solar_os_native_library_acquire("example", 7, sizeof(api), &token, &p) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    return fail_shutdown ? SOLAR_OS_NATIVE_ERROR_INVALID_STATE : SOLAR_OS_NATIVE_OK;
}
static int shutdown_builtin(void) { builtin_shutdowns++; return 0; }
static int builtin_entry(int argc, char **argv) {
    assert(argc == 1 && argv && !strcmp(argv[0], "tinycc"));
    if (fail_builtin) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    const solar_os_native_library_descriptor_v1_t descriptor = {
        1, sizeof(descriptor), "tinycc", "built-in compiler", (const void *)&api, shutdown_builtin,
    };
    builtin_loads++;
    return solar_os_native_register_library(&descriptor);
}
typedef struct { bool live; } fake_image_t;
esp_err_t solar_os_native_image_load(const char *path, void **out, char *detail, size_t length)
{
    (void)detail; (void)length;
    *out = NULL;
    if (!strcmp(path, "missing")) return ESP_ERR_NOT_FOUND;
    if (fail_load) return ESP_FAIL;
    fake_image_t *image = solar_os_memory_calloc(1, sizeof(*image), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "test.image");
    if (!image) return ESP_ERR_NO_MEM;
    image->live = true; *out = image;
    atomic_fetch_add(&images, 1); atomic_fetch_add(&loads, 1);
    return ESP_OK;
}
int solar_os_native_image_request(void *image, int argc, char **argv)
{
    assert(image && ((fake_image_t *)image)->live && argc == 1 && argv && argv[0]);
    const solar_os_native_library_descriptor_v1_t descriptor = {
        1, sizeof(descriptor), bad_registration ? "wrong-id" : argv[0],
        "example library", (const void *)&api, shutdown_library,
    };
    const int result = solar_os_native_register_library(&descriptor);
    if (!result) assert(solar_os_native_register_library(&descriptor) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    return result;
}
void solar_os_native_image_close(void *image)
{
    assert(image && ((fake_image_t *)image)->live);
    ((fake_image_t *)image)->live = false;
    atomic_fetch_sub(&images, 1); solar_os_memory_free(image);
}
esp_err_t solar_os_jobs_register_dynamic(const char *id, const char *summary, const solar_os_job_t *job)
{ (void)id; (void)summary; (void)job; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t solar_os_jobs_unregister_dynamic(const char *id, const solar_os_job_t *job)
{ (void)id; (void)job; return ESP_OK; }
/* Compile the complete production runtime. Internal access below exercises
 * refusal and reclamation races without substituting lifecycle functions. */
#include "../../src/services/solar_os_native_runtime.c"

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
    assert(solar_os_native_register_library(&descriptor) == -2);
    native_registering_slot = &slot;
    descriptor.abi_version = 2; assert(solar_os_native_register_library(&descriptor) == -2);
    descriptor.abi_version = 1; descriptor.struct_size--;
    assert(solar_os_native_register_library(&descriptor) == -2); descriptor.struct_size++;
    descriptor.shutdown = NULL; assert(solar_os_native_register_library(&descriptor) == -2);
    descriptor.shutdown = shutdown_library;
    solar_os_native_library_api_header_v1_t bad = {0, sizeof(bad)};
    descriptor.api = &bad; assert(solar_os_native_register_library(&descriptor) == -2);
    bad.abi_version = 1; bad.struct_size--;
    assert(solar_os_native_register_library(&descriptor) == -2);
    native_registering_slot = NULL;
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
    fail_reclaimer = 1;
    assert(solar_os_native_module_activate(SOLAR_OS_MODULE_TYPE_LIBRARY, "tinycc", NULL, NULL, 0) == ESP_ERR_NO_MEM);
    assert(!native_running && !native_operation_locked && !allocations);
    fail_reclaimer = 0;
    test_registration();
    test_lifecycle();
    uint32_t builtin_lease; const void *builtin_table;
    fail_builtin = 1;
    assert(solar_os_native_library_acquire("tinycc", 7, sizeof(api), &builtin_lease, &builtin_table) == SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
    assert(!builtin_lease && !builtin_table && !native_registering_slot);
    fail_builtin = 0;
    assert(!solar_os_native_library_acquire("tinycc", 7, sizeof(api), &builtin_lease, &builtin_table));
    assert(builtin_table == &api && builtin_loads == 1 && !images);
    assert(solar_os_native_library_unload("tinycc") == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!solar_os_native_library_release(builtin_lease));
    assert(!solar_os_native_library_unload("tinycc") && builtin_shutdowns == 1);
    uint32_t lease; const void *table;
    assert(solar_os_native_library_acquire("example", 7, sizeof(api), &lease, &table) == 0);
    native_resident_slot_t retained_app = {.type = SOLAR_OS_MODULE_TYPE_APP, .id = "retained"};
    assert(native_slot_add(&retained_app));
    assert(native_reclaim_unused_libraries() == 0); /* Active client protects its provider. */
    assert(solar_os_native_library_release(lease) == 0);
    fail_shutdown = 1;
    assert(native_reclaim_unused_libraries() == 0); /* Failed shutdown keeps code resident. */
    fail_shutdown = 0;
    /* A different loader task still excludes reclamation. The owner can
     * reclaim inside its operation without releasing the global lock. */
    atomic_store(&native_running, true);
    assert(native_reclaim_unused_libraries() == 0);
    native_operation_locked = true;
    assert(native_reclaim_unused_libraries() == 1);
    assert(atomic_load(&native_running));
    native_operation_locked = false;
    atomic_store(&native_running, false);
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
