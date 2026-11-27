/* Full production player with real pthread-backed binary semaphores. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "solar_os_audio_player.h"
#include "solar_os_native_client.h"
#include "solar_os_native_audio_player.h"
#include "solar_os_native_runtime.h"
#include "solar_os_memory.h"
#include "solar_os_task.h"
#include <freertos/semphr.h>

struct test_semaphore { pthread_mutex_t mutex; pthread_cond_t cond; unsigned count; bool binary; };
static atomic_uint waits, signals, allocations, samples, writes;
static atomic_bool hold_sink, close_busy, sink_wait_timeout;
static atomic_uint close_calls, active_library_leases;
static _Thread_local TaskHandle_t current_task;
static _Thread_local uint8_t direct_task;
static atomic_uint stalled_sink;
static unsigned fail_at, allocation_calls;
static void *allocate(size_t size)
{
    if (++allocation_calls == fail_at) return NULL;
    void *p = calloc(1, size);
    if (p) atomic_fetch_add(&allocations, 1);
    return p;
}
void *solar_os_memory_alloc(size_t n, solar_os_memory_class_t cls, const char *tag)
{ (void)cls; (void)tag; return allocate(n); }
void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t cls, const char *tag)
{ (void)cls; (void)tag; return allocate(count * size); }
void solar_os_memory_free(void *p)
{ if (p) { assert(atomic_fetch_sub(&allocations, 1)); free(p); } }
static SemaphoreHandle_t semaphore(unsigned count, bool binary)
{
    SemaphoreHandle_t s = allocate(sizeof(*s)); if (!s) return NULL;
    assert(!pthread_mutex_init(&s->mutex, NULL)); assert(!pthread_cond_init(&s->cond, NULL));
    s->count = count; s->binary = binary; return s;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return semaphore(1, false); }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return semaphore(0, true); }
int xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
    assert(s); pthread_mutex_lock(&s->mutex);
    struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += (long)(ticks % 1000U) * 1000000L;
    deadline.tv_sec += ticks / 1000U + deadline.tv_nsec / 1000000000L;
    deadline.tv_nsec %= 1000000000L;
    while (!s->count) {
        if (!ticks) { pthread_mutex_unlock(&s->mutex); return 0; }
        if (s->binary) atomic_fetch_add(&waits, 1);
        int result = ticks == portMAX_DELAY ? pthread_cond_wait(&s->cond, &s->mutex) :
            pthread_cond_timedwait(&s->cond, &s->mutex, &deadline);
        assert(!result || result == ETIMEDOUT);
        if (result == ETIMEDOUT) { pthread_mutex_unlock(&s->mutex); return 0; }
    }
    s->count--; pthread_mutex_unlock(&s->mutex); return 1;
}
int xSemaphoreGive(SemaphoreHandle_t s)
{
    assert(s); pthread_mutex_lock(&s->mutex); s->count = 1;
    if (s->binary) atomic_fetch_add(&signals, 1);
    pthread_cond_signal(&s->cond); pthread_mutex_unlock(&s->mutex); return 1;
}
void vSemaphoreDelete(SemaphoreHandle_t s)
{ assert(!pthread_cond_destroy(&s->cond)); assert(!pthread_mutex_destroy(&s->mutex)); solar_os_memory_free(s); }
void vSemaphoreDeleteWithCaps(SemaphoreHandle_t s) { vSemaphoreDelete(s); }
void vTaskDelay(TickType_t ticks)
{
    struct timespec delay = {.tv_sec = ticks / 1000U, .tv_nsec = (long)(ticks % 1000U) * 1000000L};
    nanosleep(&delay, NULL);
}
struct task { pthread_t thread; void (*fn)(void *); void *user; };
static void *run_task(void *arg) { struct task *t = arg; current_task = t; t->fn(t->user); return NULL; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current_task ? current_task : &direct_task; }
BaseType_t solar_os_task_create_pinned_internal(void (*fn)(void *), const char *name,
    unsigned stack, void *user, unsigned priority, TaskHandle_t *out, int core, int role)
{
    (void)name; (void)stack; (void)priority; (void)core; (void)role;
    struct task *t = allocate(sizeof(*t)); if (!t) return 0;
    t->fn = fn; t->user = user; *out = t;
    assert(!pthread_create(&t->thread, NULL, run_task, t)); return pdPASS;
}
void solar_os_task_delete_internal(TaskHandle_t task) { assert(!task); pthread_exit(NULL); }
bool solar_os_task_wait_done(TaskHandle_t task, volatile bool *done, uint32_t timeout)
{
    (void)timeout;
    if (atomic_load(&sink_wait_timeout)) return false;
    struct task *t = task;
    assert(!pthread_join(t->thread, NULL));
    assert(*done);
    solar_os_memory_free(t);
    return true;
}
size_t strlcpy(char *out, const char *in, size_t cap)
{ size_t n = strlen(in); if (cap) { size_t copy = n < cap - 1 ? n : cap - 1; memcpy(out, in, copy); out[copy] = 0; } return n; }
esp_err_t solar_os_audio_open_default(solar_os_stream_direction_t direction, const char *owner,
    const solar_os_stream_open_options_t *options, solar_os_stream_handle_t *stream, solar_os_audio_device_info_t *device)
{
    (void)direction; (void)owner;
    if (device) {
        memset(device, 0, sizeof(*device));
        strcpy(device->id, "test-output");
    }
    stream->slot = 1;
    stream->context = xTaskGetCurrentTaskHandle();
    stream->audio = options->requested_audio;
    return ESP_OK;
}
esp_err_t solar_os_audio_set_device_volume(const char *device, uint8_t volume)
{ (void)device; (void)volume; return ESP_OK; }
esp_err_t solar_os_stream_write(solar_os_stream_handle_t *stream, const void *data,
    size_t len, uint32_t timeout, size_t *written)
{
    assert(stream->context == xTaskGetCurrentTaskHandle());
    (void)data; (void)timeout;
    if (atomic_load(&hold_sink)) {
        atomic_fetch_add(&stalled_sink, 1);
        while (atomic_load(&hold_sink)) vTaskDelay(1);
    }
    *written = len; atomic_fetch_add(&writes, 1); vTaskDelay(1); return ESP_OK;
}
esp_err_t solar_os_stream_close_ex(solar_os_stream_handle_t *stream)
{
    assert(stream->context == xTaskGetCurrentTaskHandle());
    atomic_fetch_add(&close_calls, 1);
    if (atomic_load(&close_busy)) return ESP_ERR_INVALID_STATE;
    *stream = (solar_os_stream_handle_t)SOLAR_OS_STREAM_HANDLE_INIT;
    return ESP_OK;
}

int solar_os_native_library_retain(solar_os_native_handle_t lease, solar_os_native_handle_t *out)
{
    assert(lease == 123);
    atomic_fetch_add(&active_library_leases, 1);
    *out = 456;
    return SOLAR_OS_NATIVE_OK;
}
int solar_os_native_library_release(solar_os_native_handle_t lease)
{
    assert(lease == 456 && atomic_fetch_sub(&active_library_leases, 1));
    return SOLAR_OS_NATIVE_OK;
}
static void note_samples(const int16_t *data, size_t count, uint8_t channels, void *user)
{ (void)data; (void)user; assert(channels == 1); atomic_fetch_add(&samples, count); }
static solar_os_audio_player_options_t options(size_t capacity)
{
    return (solar_os_audio_player_options_t){.owner = "test", .volume = SOLAR_OS_AUDIO_VOLUME_GLOBAL,
        .buffered = true, .internal_buffer_bytes = capacity, .target_ms = 500, .samples = note_samples,
        .requested_audio = {.sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE, .sample_rate = 48000,
            .channels = 1, .bits_per_sample = 16, .frames_per_block = 128}};
}
typedef struct { solar_os_audio_player_t *player; const int16_t *data; volatile bool cancel; esp_err_t error; } writer_t;
static void *write_until_cancel(void *user)
{
    writer_t *writer = user;
    writer->error = solar_os_audio_player_write(writer->player, writer->data, 16384, &writer->cancel);
    return NULL;
}
static const solar_os_native_audio_player_api_v1_t *native;

static void *close_from_other_task(void *opaque)
{
    const solar_os_native_handle_t *native_player = opaque;
    assert(native->close(*native_player) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    return NULL;
}

static void check_native_close(const int16_t *data, bool buffered)
{
    native = solar_os_native_audio_player_get(solar_os_native_audio_player_get_service, sizeof(*native));
    assert(native);
    solar_os_native_audio_player_options_v1_t options = {
        .owner = "native", .format = {.sample_rate = 48000, .channels = 1, .bits_per_sample = 16},
        .buffered = buffered, .internal_buffer_bytes = 4096, .volume = SOLAR_OS_NATIVE_AUDIO_PLAYER_VOLUME_GLOBAL,
        .open_timeout_ms = 100, .samples = note_samples,
    };
    solar_os_native_handle_t player;
    assert(native->open(&options, &player, NULL, NULL) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !player);
    options.library_lease = 123;
    solar_os_native_stream_format_v1_t negotiated;
    char device[SOLAR_OS_AUDIO_DEVICE_ID_MAX];
    assert(!native->open(&options, &player, &negotiated, device));
    assert(player && negotiated.sample_rate == 48000 && !strcmp(device, "test-output"));
    assert(atomic_load(&active_library_leases) == 1);
    assert(!native->write(player, data, 128));
    if (!buffered) {
        pthread_t other;
        assert(!pthread_create(&other, NULL, close_from_other_task, &player));
        assert(!pthread_join(other, NULL));
    }
    const unsigned preceding_calls = atomic_load(&close_calls);
    atomic_store(&close_busy, true);
    assert(!native->end_input(player));
    atomic_store(&sink_wait_timeout, buffered);
    assert(native->close(player) == (buffered ? SOLAR_OS_NATIVE_ERROR_TIMEOUT : SOLAR_OS_NATIVE_ERROR_INVALID_STATE));
    assert(atomic_load(&active_library_leases) == 1);
    if (buffered) {
        for (unsigned attempt = 0; attempt < 1000 && atomic_load(&close_calls) == preceding_calls; ++attempt)
            vTaskDelay(1);
        assert(atomic_load(&close_calls) > preceding_calls);
    }
    atomic_store(&close_busy, false);
    atomic_store(&sink_wait_timeout, false);
    assert(!native->close(player));
    assert(!atomic_load(&active_library_leases) && !atomic_load(&allocations));
    assert(native->close(player) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
}

int main(void)
{
    int16_t data[12001]; for (size_t i = 0; i < 12001; i++) data[i] = (int16_t)i;
    /* Partial construction must release every semaphore and buffer. */
    for (unsigned fail = 1; fail <= 8; fail++) {
        allocation_calls = 0; fail_at = fail;
        solar_os_audio_player_t *player = NULL; solar_os_audio_player_options_t opts = options(8192);
        assert(solar_os_audio_player_create(&opts, &player, NULL, NULL) != ESP_OK);
        assert(!player && !atomic_load(&allocations));
    }
    fail_at = 0;
    {
        solar_os_audio_player_options_t opts = options(8192);
        solar_os_audio_player_t *player = NULL;
        assert(!solar_os_audio_player_create(&opts, &player, NULL, NULL));
        atomic_store(&hold_sink, true);
        unsigned previous_waits = atomic_load(&waits);
        writer_t writer = {.player = player, .data = data};
        pthread_t producer; assert(!pthread_create(&producer, NULL, write_until_cancel, &writer));
        for (unsigned attempt = 0; attempt < 2000 &&
            (!atomic_load(&stalled_sink) || atomic_load(&waits) == previous_waits); attempt++) vTaskDelay(1);
        assert(atomic_load(&stalled_sink) && atomic_load(&waits) > previous_waits);
        writer.cancel = true;
        assert(!pthread_join(producer, NULL));
        assert(writer.error == ESP_ERR_TIMEOUT);
        atomic_store(&hold_sink, false);
        solar_os_audio_player_destroy(player); assert(!atomic_load(&allocations));
    }
    for (unsigned cycle = 0; cycle < 6; cycle++) {
        /* A sub-block ring must also prime and stream without deadlocking. */
        solar_os_audio_player_options_t opts = options(cycle % 2 ? 8192 : 128);
        solar_os_audio_player_t *player = NULL; atomic_store(&samples, 0);
        assert(solar_os_audio_player_create(&opts, &player, NULL, NULL) == ESP_OK);
        assert(solar_os_audio_player_write(player, data, sizeof(data), NULL) == ESP_OK);
        assert(solar_os_audio_player_end_input(player) == ESP_OK);
        assert(solar_os_audio_player_finish(player, NULL) == ESP_OK);
        assert(atomic_load(&samples) == 12001);
        solar_os_audio_player_destroy(player); assert(!atomic_load(&allocations));
    }
    for (unsigned cycle = 0; cycle < 2; cycle++) {
        solar_os_audio_player_options_t opts = options(8192);
        solar_os_audio_player_t *player = NULL;
        assert(!solar_os_audio_player_create(&opts, &player, NULL, NULL));
        if (cycle) assert(!solar_os_audio_player_finish(player, NULL));
        /* Destroy/EOF wake the sink even if it never received a byte. */
        solar_os_audio_player_destroy(player); assert(!atomic_load(&allocations));
    }
    check_native_close(data, false);
    check_native_close(data, true);
    assert(atomic_load(&waits) && atomic_load(&signals) && atomic_load(&writes));
}
