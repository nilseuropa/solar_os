#include "solar_os_native_queue.h"
#include "solar_os_config.h"
#include <string.h>

#if SOLAR_OS_PACKAGE_CORE_RUNTIME
#include "solar_os.h"
#include "solar_os_memory.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_queue_abi.h"
#include "solar_os_queue.h"

#define QUEUE_HANDLE_KIND SOLAR_OS_NATIVE_HANDLE_QUEUE

typedef struct {
    QueueHandle_t queue;
    uint32_t bytes, depth;
    bool internal;
} native_queue_t;

static int queue_destroy(void *opaque)
{
    native_queue_t *state = opaque;
    if (state->internal) solar_os_queue_delete_internal(state->queue);
    else solar_os_queue_delete(state->queue);
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static int queue_open(uint32_t depth, uint32_t bytes, bool internal, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    if (!depth || !bytes || depth > SIZE_MAX / bytes) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_queue_t *state = solar_os_memory_calloc(1, sizeof(*state), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "native.queue");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    state->internal = internal;
    state->bytes = bytes;
    state->depth = depth;
    state->queue = internal ? solar_os_queue_create_internal(depth, bytes) : solar_os_queue_create(depth, bytes);
    if (!state->queue) {
        solar_os_memory_free(state);
        return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    }
    const solar_os_app_t *owner = solar_os_application_descriptor();
    const int result = owner ? solar_os_native_handle_create_owned(QUEUE_HANDLE_KIND, state, queue_destroy, owner, out) :
        solar_os_native_handle_create(QUEUE_HANDLE_KIND, state, queue_destroy, out);
    if (result) (void)queue_destroy(state);
    return result;
}

static int queue_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id, QUEUE_HANDLE_KIND); }

static TickType_t queue_wait(uint32_t timeout_ms)
{
    if (timeout_ms == UINT32_MAX) return portMAX_DELAY;
    const uint64_t ticks = ((uint64_t)timeout_ms * configTICK_RATE_HZ + 999U) / 1000U;
    return ticks < portMAX_DELAY ? (TickType_t)ticks : portMAX_DELAY - 1U;
}

static int queue_send(solar_os_native_handle_t id, const void *item, size_t size, uint32_t timeout_ms)
{
    if (!item) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_queue_t *state = solar_os_native_handle_acquire(id, QUEUE_HANDLE_KIND);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    if (size == state->bytes)
        result = xQueueSend(state->queue, item, queue_wait(timeout_ms)) == pdTRUE ?
            SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_TIMEOUT;
    solar_os_native_handle_release(id);
    return result;
}

static int queue_read(solar_os_native_handle_t id, void *item, size_t size, uint32_t timeout_ms, bool peek)
{
    if (!item) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_queue_t *state = solar_os_native_handle_acquire(id, QUEUE_HANDLE_KIND);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    if (size == state->bytes) {
        const TickType_t wait = queue_wait(timeout_ms);
        const BaseType_t ready = peek ? xQueuePeek(state->queue, item, wait) : xQueueReceive(state->queue, item, wait);
        result = ready == pdTRUE ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_TIMEOUT;
    }
    solar_os_native_handle_release(id);
    return result;
}

static int queue_receive(solar_os_native_handle_t id, void *item, size_t size, uint32_t timeout_ms)
{ return queue_read(id, item, size, timeout_ms, false); }

static int queue_peek(solar_os_native_handle_t id, void *item, size_t size, uint32_t timeout_ms)
{ return queue_read(id, item, size, timeout_ms, true); }

static int queue_overwrite(solar_os_native_handle_t id, const void *item, size_t size)
{
    if (!item) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_queue_t *state = solar_os_native_handle_acquire(id, QUEUE_HANDLE_KIND);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    if (size == state->bytes && state->depth == 1)
        result = xQueueOverwrite(state->queue, item) == pdTRUE ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_FAILED;
    solar_os_native_handle_release(id);
    return result;
}

static int queue_count(solar_os_native_handle_t id, uint32_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    native_queue_t *state = solar_os_native_handle_acquire(id, QUEUE_HANDLE_KIND);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    *out = uxQueueMessagesWaiting(state->queue);
    solar_os_native_handle_release(id);
    return SOLAR_OS_NATIVE_OK;
}

static int queue_clear(solar_os_native_handle_t id)
{
    native_queue_t *state = solar_os_native_handle_acquire(id, QUEUE_HANDLE_KIND);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const BaseType_t result = xQueueReset(state->queue);
    solar_os_native_handle_release(id);
    return result == pdPASS ? SOLAR_OS_NATIVE_OK : SOLAR_OS_NATIVE_ERROR_FAILED;
}

static const solar_os_native_queue_api_v1_t queue_api = {
    .abi_version = SOLAR_OS_NATIVE_QUEUE_ABI, .struct_size = sizeof(queue_api),
    .open = queue_open, .close = queue_close, .send = queue_send, .receive = queue_receive,
    .peek = queue_peek, .overwrite = queue_overwrite, .count = queue_count, .clear = queue_clear,
};
#endif

const void *solar_os_native_queue_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_queue_dispatch.inc"
    (void)name; (void)abi; (void)minimum;
    return NULL;
}
