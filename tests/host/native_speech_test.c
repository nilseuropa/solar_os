#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "solar_os.h"
#include "solar_os_memory.h"
#include "solar_os_native_speech.h"
#include "solar_os_speech.h"

atomic_uint speech_test_waiters;
static size_t allocations;
static const solar_os_native_speech_worker_api_v1_t *worker;
static solar_os_native_handle_t handle;

const solar_os_app_t *solar_os_application_descriptor(void) { return NULL; }

void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t cls, const char *tag)
{
    (void)cls;
    (void)tag;
    void *out = malloc(size);
    if (out) allocations++;
    return out;
}

void solar_os_memory_free(void *ptr)
{
    if (ptr) allocations--;
    free(ptr);
}

size_t strlcpy(char *out, const char *input, size_t capacity)
{
    const size_t length = strlen(input);
    if (capacity) {
        const size_t copied = length < capacity ? length : capacity - 1;
        memcpy(out, input, copied);
        out[copied] = 0;
    }
    return length;
}

static void *blocking_take(void *unused)
{
    (void)unused;
    solar_os_speech_work_t work;
    assert(worker->take(handle, &work, UINT32_MAX) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !work.id);
    return NULL;
}

int main(void)
{
    const solar_os_native_speech_api_v1_t *speech =
        solar_os_native_speech_get_service("speech", 1, sizeof(*speech));
    worker = solar_os_native_speech_get_service("speech.worker", 1, sizeof(*worker));
    assert(speech && worker && !solar_os_native_speech_get_service("speech.worker", 2, sizeof(*worker)));
    static int builtin;
    assert(!solar_os_speech_worker_start(&builtin));
    assert(worker->open(&handle) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !handle && !allocations);
    assert(!solar_os_speech_worker_stop(&builtin));
    assert(worker->open(&handle) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !handle);
    assert(!solar_os_speech_worker_detach(&builtin));
    assert(!worker->open(&handle) && handle);
    assert(solar_os_speech_worker_stop(&builtin) == ESP_ERR_NOT_FOUND);
    char text[] = "hello";
    const solar_os_speech_request_t request = {.text = text, .text_len = 5, .volume = 42};
    uint32_t id;
    assert(!speech->enqueue(&request, &id));
    text[0] = 'X';
    solar_os_speech_work_t work;
    assert(!worker->take(handle, &work, 0) && work.id == id && !strcmp(work.text, "hello"));
    assert(work.pitch == SOLAR_OS_SPEECH_PITCH_DEFAULT && work.volume == 42 && !work.streaming);
    assert(worker->take(handle, &work, 0) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !work.id);
    assert(!worker->set_state(handle, id, SOLAR_OS_SPEECH_REQUEST_SPEAKING));
    assert(!worker->set_progress(handle, id, 2, 5));
    solar_os_speech_request_status_t status;
    assert(!speech->request_status(id, &status) && status.progress_done == 2);
    assert(!worker->finish(handle, id, SOLAR_OS_SPEECH_REQUEST_COMPLETE, 0));
    assert(!speech->request_status(id, &status) && status.progress_done == 5 && !status.error);

    const solar_os_speech_stream_options_t options = {.pitch = 80, .speed = 90, .volume = 90};
    assert(!speech->stream_begin(&options, &id));
    assert(!speech->stream_write(id, "a", 1, false));
    assert(speech->stream_write(id, "b", 1, true) == SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
    assert(!worker->take(handle, &work, 0) && work.streaming && work.id == id);
    char chunk[SOLAR_OS_SPEECH_TEXT_MAX + 1];
    size_t length;
    bool final;
    assert(!worker->stream_take(handle, id, chunk, sizeof(chunk), &length, &final, 0));
    assert(length == 1 && !final && !strcmp(chunk, "a"));
    assert(!speech->stream_write(id, NULL, 0, true));
    assert(!worker->stream_take(handle, id, chunk, sizeof(chunk), &length, &final, 0) && !length && final);
    assert(!speech->cancel(id));
    bool cancelled;
    assert(!worker->cancelled(handle, id, &cancelled) && cancelled);
    assert(!worker->finish(handle, id, SOLAR_OS_SPEECH_REQUEST_COMPLETE, 0));
    assert(!speech->request_status(id, &status) && status.state == SOLAR_OS_SPEECH_REQUEST_CANCELLED);

    pthread_t waiting;
    assert(!pthread_create(&waiting, NULL, blocking_take, NULL));
    for (unsigned attempt = 0; !atomic_load(&speech_test_waiters); attempt++) {
        assert(attempt < 2000);
        nanosleep(&(struct timespec){.tv_nsec = 1000000L}, NULL);
    }
    assert(worker->take(handle, &work, 0) == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(worker->close(handle) == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(!worker->stop(handle));
    assert(!pthread_join(waiting, NULL));
    solar_os_native_handle_t other;
    assert(worker->open(&other) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !other);
    assert(!worker->close(handle) && !allocations);
    assert(!worker->open(&other) && other != handle);
    assert(worker->stop(handle) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!speech->enqueue(&request, &id));
    assert(!worker->take(other, &work, 0));
    assert(!worker->close(other) && !allocations);
    assert(!speech->request_status(id, &status) && status.state == SOLAR_OS_SPEECH_REQUEST_CANCELLED);
}
