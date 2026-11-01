#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include "solar_os_sync.h"
#include "solar_os_sync_dynamic.h"
atomic_uint registry_mutex_initializations;
atomic_uint registry_dynamic_live;
pthread_barrier_t *registry_dynamic_create_barrier;
static SemaphoreHandle_t handle;
static SemaphoreHandle_t dynamic_handle;
static StaticSemaphore_t storage;
static pthread_barrier_t start;
static unsigned count, dynamic_count;
static void *worker(void *unused)
{
    (void)unused;
    pthread_barrier_wait(&start);
    for (unsigned i = 0; i < 100; ++i) {
        SemaphoreHandle_t mutex = solar_os_mutex_get(&handle, &storage);
        assert(mutex == &storage);
        assert(xSemaphoreTake(mutex, portMAX_DELAY));
        ++count;
        xSemaphoreGive(mutex);
        mutex = solar_os_mutex_get_dynamic(&dynamic_handle);
        assert(mutex != NULL);
        assert(xSemaphoreTake(mutex, portMAX_DELAY));
        ++dynamic_count;
        xSemaphoreGive(mutex);
    }
    return NULL;
}
int main(void)
{
    pthread_t threads[16];
    assert(!pthread_barrier_init(&start, NULL, 16));
    /* Hold every candidate before publication so all sixteen first callers
     * exercise the winner/loser path, including candidate destruction. */
    pthread_barrier_t candidates;
    assert(!pthread_barrier_init(&candidates, NULL, 16));
    registry_dynamic_create_barrier = &candidates;
    for (unsigned i = 0; i < 16; ++i) assert(!pthread_create(&threads[i], NULL, worker, NULL));
    for (unsigned i = 0; i < 16; ++i) assert(!pthread_join(threads[i], NULL));
    assert(count == 1600 && dynamic_count == 1600 && atomic_load(&registry_mutex_initializations) == 1);
    assert(atomic_load(&registry_dynamic_live) == 1);
    vSemaphoreDelete(dynamic_handle);
    assert(atomic_load(&registry_dynamic_live) == 0);
    pthread_barrier_destroy(&start);
    pthread_barrier_destroy(&candidates);
    pthread_mutex_destroy(&storage);
}
