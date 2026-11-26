#include "solar_os_native_speech.h"
#include "solar_os_config.h"
#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_SPEECH
#include <stdatomic.h>
#include "solar_os.h"
#include "solar_os_memory.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"
#include "solar_os_speech.h"

static int speech_enqueue(const solar_os_speech_request_t *request, uint32_t *id)
{ return solar_os_native_result(solar_os_speech_enqueue(request, id)); }

static int speech_stream_begin(const solar_os_speech_stream_options_t *options, uint32_t *id)
{ return solar_os_native_result(solar_os_speech_stream_begin(options, id)); }

static int speech_stream_write(uint32_t id, const char *text, size_t length, bool final)
{ return solar_os_native_result(solar_os_speech_stream_write(id, text, length, final)); }

static int speech_cancel(uint32_t id)
{ return solar_os_native_result(solar_os_speech_cancel(id)); }

static int speech_request_status(uint32_t id, solar_os_speech_request_status_t *status)
{
    if (!status) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *status = (solar_os_speech_request_status_t){0};
    if (!id) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (!solar_os_speech_request_status(id, status)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    status->error = solar_os_native_result(status->error);
    return SOLAR_OS_NATIVE_OK;
}

static int speech_queue_status(solar_os_speech_queue_status_t *status)
{
    if (!status) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_speech_queue_get_status(status);
    return SOLAR_OS_NATIVE_OK;
}

static const solar_os_native_speech_api_v1_t speech_api = {
    .abi_version = SOLAR_OS_NATIVE_SPEECH_ABI, .struct_size = sizeof(speech_api),
    .enqueue = speech_enqueue, .stream_begin = speech_stream_begin, .stream_write = speech_stream_write,
    .cancel = speech_cancel, .request_status = speech_request_status, .queue_status = speech_queue_status,
    .state_name = solar_os_speech_request_state_name,
};

#define SPEECH_WORKER_KIND SOLAR_OS_NATIVE_HANDLE_SPEECH_WORKER

typedef struct {
    atomic_bool reading;
    bool attached;
} native_speech_worker_t;

static int worker_destroy(void *opaque)
{
    native_speech_worker_t *state = opaque;
    if (state->attached) {
        const esp_err_t error = solar_os_speech_worker_detach(state);
        if (error != ESP_OK) return solar_os_native_result(error);
    }
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static int worker_open(solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    native_speech_worker_t *state = solar_os_memory_alloc(sizeof(*state), SOLAR_OS_MEMORY_INTERNAL_PREFERRED,
        "speech.worker");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    atomic_init(&state->reading, false);
    state->attached = false;
    const solar_os_app_t *owner = solar_os_application_descriptor();
    solar_os_native_handle_t id = 0;
    int result = owner ? solar_os_native_handle_create_owned(SPEECH_WORKER_KIND, state, worker_destroy, owner, &id) :
        solar_os_native_handle_create(SPEECH_WORKER_KIND, state, worker_destroy, &id);
    if (result) {
        solar_os_memory_free(state);
        return result;
    }
    result = solar_os_native_result(solar_os_speech_worker_start(state));
    state->attached = result == SOLAR_OS_NATIVE_OK;
    if (state->attached) *out = id;
    else (void)solar_os_native_handle_close_kind(id, SPEECH_WORKER_KIND);
    return result;
}

static int worker_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id, SPEECH_WORKER_KIND); }

static int worker_stop(solar_os_native_handle_t id)
{
    void *worker = solar_os_native_handle_acquire(id, SPEECH_WORKER_KIND);
    if (!worker) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const esp_err_t error = solar_os_speech_worker_stop(worker);
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static int worker_begin_read(solar_os_native_handle_t id, native_speech_worker_t **out)
{
    native_speech_worker_t *state = solar_os_native_handle_acquire(id, SPEECH_WORKER_KIND);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if (atomic_exchange(&state->reading, true)) {
        solar_os_native_handle_release(id);
        return SOLAR_OS_NATIVE_ERROR_BUSY;
    }
    *out = state;
    return SOLAR_OS_NATIVE_OK;
}

static int worker_finish_read(solar_os_native_handle_t id, native_speech_worker_t *state, esp_err_t error)
{
    atomic_store(&state->reading, false);
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static int worker_take(solar_os_native_handle_t id, solar_os_speech_work_t *work, uint32_t timeout_ms)
{
    if (!work) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *work = (solar_os_speech_work_t){0};
    native_speech_worker_t *state;
    const int result = worker_begin_read(id, &state);
    return result ? result : worker_finish_read(id, state, solar_os_speech_worker_take(work, timeout_ms));
}

static int worker_stream_take(solar_os_native_handle_t id, uint32_t request, char *text,
    size_t capacity, size_t *length, bool *final, uint32_t timeout_ms)
{
    if (!length || !final || !text) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *length = 0;
    *final = false;
    if (capacity) text[0] = 0;
    native_speech_worker_t *state;
    const int result = worker_begin_read(id, &state);
    return result ? result : worker_finish_read(id, state,
        solar_os_speech_worker_stream_take(request, text, capacity, length, final, timeout_ms));
}

static int worker_set_state(solar_os_native_handle_t id, uint32_t request, solar_os_speech_request_state_t state)
{
    if (state != SOLAR_OS_SPEECH_REQUEST_WAITING_AUDIO && state != SOLAR_OS_SPEECH_REQUEST_SPEAKING)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (!solar_os_native_handle_acquire(id, SPEECH_WORKER_KIND)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const esp_err_t error = solar_os_speech_worker_set_state(request, state);
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static int worker_set_progress(solar_os_native_handle_t id, uint32_t request, size_t done, size_t total)
{
    if (!solar_os_native_handle_acquire(id, SPEECH_WORKER_KIND)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const esp_err_t error = solar_os_speech_worker_set_progress(request, done, total);
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static int worker_finish(solar_os_native_handle_t id, uint32_t request, solar_os_speech_request_state_t state,
    int reason)
{
    if ((unsigned)state < SOLAR_OS_SPEECH_REQUEST_COMPLETE || (unsigned)state > SOLAR_OS_SPEECH_REQUEST_FAILED)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (!solar_os_native_handle_acquire(id, SPEECH_WORKER_KIND)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const esp_err_t error = solar_os_speech_worker_finish(request, state, solar_os_native_callback_result(reason));
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static int worker_cancelled(solar_os_native_handle_t id, uint32_t request, bool *cancelled)
{
    if (!cancelled || !request) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *cancelled = false;
    if (!solar_os_native_handle_acquire(id, SPEECH_WORKER_KIND)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    *cancelled = solar_os_speech_worker_cancelled(request);
    solar_os_native_handle_release(id);
    return SOLAR_OS_NATIVE_OK;
}

static const solar_os_native_speech_worker_api_v1_t speech_worker_api = {
    .abi_version = SOLAR_OS_NATIVE_SPEECH_WORKER_ABI, .struct_size = sizeof(speech_worker_api),
    .open = worker_open, .stop = worker_stop, .close = worker_close, .take = worker_take,
    .stream_take = worker_stream_take, .set_state = worker_set_state, .set_progress = worker_set_progress,
    .finish = worker_finish, .cancelled = worker_cancelled,
};
#endif

const void *solar_os_native_speech_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_speech_dispatch.inc"
    (void)name; (void)abi; (void)minimum;
    return NULL;
}
