#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "solar_os.h"
#include "solar_os_memory.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_queue.h"
#include "solar_os_native_queue_abi.h"
#include "solar_os_queue.h"

struct test_queue {
    pthread_mutex_t lock;
    pthread_cond_t readable;
    uint32_t items[4], depth, count;
    bool internal;
};

static size_t adapters, queues;
static atomic_bool waiting;
static TickType_t last_timeout;
static const solar_os_app_t app;
static const solar_os_native_queue_api_v1_t *api;

const solar_os_app_t *solar_os_application_descriptor(void) { return &app; }

void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t cls, const char *tag)
{
    (void)cls;
    (void)tag;
    void *value = calloc(n, size);
    if (value) adapters++;
    return value;
}

void solar_os_memory_free(void *value)
{
    if (value) adapters--;
    free(value);
}

static QueueHandle_t test_open(uint32_t depth, uint32_t size, bool internal)
{
    assert(size == sizeof(uint32_t));
    if (depth > 4) return NULL;
    QueueHandle_t queue = calloc(1, sizeof(*queue));
    assert(queue && !pthread_mutex_init(&queue->lock, NULL) && !pthread_cond_init(&queue->readable, NULL));
    queue->depth = depth;
    queue->internal = internal;
    queues++;
    return queue;
}

QueueHandle_t solar_os_queue_create(UBaseType_t depth, UBaseType_t size) { return test_open(depth, size, false); }
QueueHandle_t solar_os_queue_create_internal(UBaseType_t depth, UBaseType_t size) { return test_open(depth, size, true); }

static void test_close(QueueHandle_t queue, bool internal)
{
    assert(queue && queue->internal == internal && !atomic_load(&waiting));
    assert(!pthread_mutex_destroy(&queue->lock) && !pthread_cond_destroy(&queue->readable));
    queues--;
    free(queue);
}

void solar_os_queue_delete(QueueHandle_t queue) { test_close(queue, false); }
void solar_os_queue_delete_internal(QueueHandle_t queue) { test_close(queue, true); }

BaseType_t xQueueSend(QueueHandle_t queue, const void *value, TickType_t wait)
{
    assert(!pthread_mutex_lock(&queue->lock));
    last_timeout = wait;
    const bool room = queue->count < queue->depth;
    if (room) {
        queue->items[queue->count++] = *(const uint32_t *)value;
        assert(!pthread_cond_signal(&queue->readable));
    }
    assert(!pthread_mutex_unlock(&queue->lock));
    return room ? pdTRUE : pdFALSE;
}

static BaseType_t test_read(QueueHandle_t queue, void *out, TickType_t wait, bool remove)
{
    assert(!pthread_mutex_lock(&queue->lock));
    last_timeout = wait;
    while (!queue->count && wait == portMAX_DELAY) {
        atomic_store(&waiting, true);
        assert(!pthread_cond_wait(&queue->readable, &queue->lock));
        atomic_store(&waiting, false);
    }
    bool available = queue->count != 0;
    if (available) {
        *(uint32_t *)out = queue->items[0];
        if (remove) memmove(queue->items, queue->items + 1, --queue->count * sizeof(queue->items[0]));
    }
    assert(!pthread_mutex_unlock(&queue->lock));
    return available ? pdTRUE : pdFALSE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait)
{ return test_read(queue, item, wait, true); }
BaseType_t xQueuePeek(QueueHandle_t queue, void *item, TickType_t wait)
{ return test_read(queue, item, wait, false); }
BaseType_t xQueueOverwrite(QueueHandle_t queue, const void *item)
{
    assert(queue->depth == 1);
    queue->count = 1;
    queue->items[0] = *(const uint32_t *)item;
    return pdTRUE;
}
BaseType_t xQueueReset(QueueHandle_t queue) { queue->count = 0; return pdPASS; }
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue) { return queue->count; }

static void *receive(void *handle)
{
    uint32_t value;
    assert(!api->receive(*(solar_os_native_handle_t *)handle, &value, sizeof(value), UINT32_MAX));
    assert(value == 42);
    return NULL;
}

int main(void)
{
    api = solar_os_native_queue_get_service("queue", 1, sizeof(*api));
    assert(api && !solar_os_native_queue_get_service("queue", 2, sizeof(*api)));
    solar_os_native_handle_t id;
    uint32_t value = 16, out = 0, count;
    assert(api->open(0, sizeof(value), false, &id) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !id);
    assert(api->open(5, sizeof(value), true, &id) == SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !id && !adapters);
    assert(!api->open(2, sizeof(value), false, &id));
    assert(api->overwrite(id, &value, sizeof(value)) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE);
    assert(api->send(id, &value, sizeof(value) - 1, 0) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE);
    assert(!api->send(id, &value, sizeof(value), 11) && last_timeout == 2);
    value = 20;
    assert(!api->send(id, &value, sizeof(value), 0));
    assert(api->send(id, &value, sizeof(value), 0) == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    assert(!api->peek(id, &out, sizeof(out), 0) && out == 16);
    assert(!api->count(id, &count) && count == 2);
    assert(!api->receive(id, &out, sizeof(out), 0) && out == 16);
    assert(!api->receive(id, &out, sizeof(out), 0) && out == 20);
    assert(api->receive(id, &out, sizeof(out), 1) == SOLAR_OS_NATIVE_ERROR_TIMEOUT && last_timeout == 1);
    pthread_t reader;
    assert(!pthread_create(&reader, NULL, receive, &id));
    for (unsigned attempt = 0; !atomic_load(&waiting); attempt++) {
        assert(attempt < 2000);
        nanosleep(&(struct timespec){.tv_nsec = 1000000L}, NULL);
    }
    assert(api->close(id) == SOLAR_OS_NATIVE_ERROR_BUSY);
    value = 42;
    assert(!api->send(id, &value, sizeof(value), 0));
    assert(!pthread_join(reader, NULL));
    assert(!api->close(id) && !adapters && !queues);
    assert(api->count(id, &count) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !count);

    assert(!api->open(1, sizeof(value), true, &id));
    assert(!api->send(id, &value, sizeof(value), 0));
    value++;
    assert(!api->overwrite(id, &value, sizeof(value)));
    assert(!api->peek(id, &out, sizeof(out), 0) && out == 43);
    assert(!api->clear(id) && !api->count(id, &count) && !count);
    assert(!solar_os_native_handle_close_owner(&app) && !adapters && !queues);
}
