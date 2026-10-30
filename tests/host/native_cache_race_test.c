#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>
#include "solar_os_memory.h"

typedef struct {
    atomic_bool cache_ready, cache_reclaiming, stop_requested;
    size_t (*cache_release)(void *user);
    void *cache_user;
} foreground_state_t;
static void foreground_sleep_ms(uint32_t ms) { (void)ms; sched_yield(); }
#include "native_cache.inc"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static bool entered, release;
static atomic_bool completed, resumed;
static foreground_state_t state;
static size_t cache_release(void *user)
{
    assert(user == &state);
    pthread_mutex_lock(&lock);
    entered = true;
    pthread_cond_broadcast(&condition);
    while (!release) pthread_cond_wait(&condition, &lock);
    pthread_mutex_unlock(&lock);
    atomic_store(&completed, true);
    return 8192;
}
static void *pressure(void *user)
{ assert(foreground_cache_reclaim(NULL, user) == 8192); return NULL; }
static void *resume_guest(void *user)
{
    foreground_cache_close(user);
    assert(atomic_load(&completed));
    atomic_store(&resumed, true);
    return NULL;
}
int main(void)
{
    alarm(10);
    atomic_init(&state.cache_ready, true);
    state.cache_release = cache_release; state.cache_user = &state;
    pthread_t reclaim_thread, resume_thread;
    assert(!pthread_create(&reclaim_thread, NULL, pressure, &state));
    pthread_mutex_lock(&lock);
    while (!entered) pthread_cond_wait(&condition, &lock);
    pthread_mutex_unlock(&lock);
    assert(!pthread_create(&resume_thread, NULL, resume_guest, &state));
    while (atomic_load(&state.cache_ready)) sched_yield();
    assert(!atomic_load(&resumed));
    assert(foreground_cache_reclaim(NULL, &state) == 0);
    pthread_mutex_lock(&lock);
    release = true; pthread_cond_broadcast(&condition);
    pthread_mutex_unlock(&lock);
    pthread_join(reclaim_thread, NULL); pthread_join(resume_thread, NULL);
    assert(atomic_load(&resumed) && !atomic_load(&state.cache_reclaiming));
    assert(foreground_cache_reclaim(NULL, &state) == 0);
    puts("native cache race: resume waits for reclamation and closes the callback gate");
}
