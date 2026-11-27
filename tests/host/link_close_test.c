#include "solar_os.h"
#include "solar_os_memory.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_link_repeater_abi.h"
#include "solar_os_native_link.h"
#include "solar_os_native_link_abi.h"
#include "solar_os_queue.h"
#include "solar_os_link.h"
#include "solar_os_meshcore_stream.h"
#include "solar_os_contacts.h"
#include "solar_os_messaging.h"
#include "solar_os_native_client.h"
#include "solar_os_port.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

struct test_queue {
    size_t capacity;
    size_t item_size;
    size_t count;
    size_t head;
    unsigned char entries[];
};
static unsigned queues;
static TickType_t ticks;
static bool destroy_while_receiving;
static solar_os_port_driver_t port;
static bool port_open;

TickType_t xTaskGetTickCount(void) { return ticks; }
void vTaskDelay(TickType_t duration) { ticks += duration; }

QueueHandle_t solar_os_queue_create(UBaseType_t length, UBaseType_t size)
{
    QueueHandle_t queue = calloc(1, sizeof(*queue) + length * size);
    assert(queue && length && size);
    queue->capacity = length;
    queue->item_size = size;
    ++queues;
    return queue;
}

void solar_os_queue_delete(QueueHandle_t queue)
{
    assert(queue && queues);
    --queues;
    free(queue);
}

unsigned uxQueueSpacesAvailable(QueueHandle_t queue) { return queue->capacity - queue->count; }
unsigned uxQueueMessagesWaiting(QueueHandle_t queue) { return queue->count; }

int xQueueSend(QueueHandle_t queue, const void *item, unsigned timeout)
{
    (void)timeout;
    if (queue->count == queue->capacity) return pdFALSE;
    const size_t index = (queue->head + queue->count++) % queue->capacity;
    memcpy(queue->entries + index * queue->item_size, item, queue->item_size);
    return pdTRUE;
}

int xQueueReceive(QueueHandle_t queue, void *item, unsigned timeout)
{
    (void)timeout;
    if (destroy_while_receiving) {
        destroy_while_receiving = false;
        assert(solar_os_link_destroy("radio") == ESP_ERR_TIMEOUT);
        assert(!solar_os_link_count() && queues == 2);
    }
    if (!queue->count) return pdFALSE;
    memcpy(item, queue->entries + queue->head * queue->item_size, queue->item_size);
    queue->head = (queue->head + 1) % queue->capacity;
    --queue->count;
    return pdTRUE;
}

void xQueueReset(QueueHandle_t queue)
{
    queue->count = queue->head = 0;
}

uint32_t esp_random(void) { return 101; }

esp_err_t solar_os_port_register(const solar_os_port_driver_t *driver)
{
    assert(!port.name && driver && driver->name && driver->label);
    port = *driver;
    return ESP_OK;
}

esp_err_t solar_os_port_unregister_many_owned(const char *const *names,
    const void *const *users, size_t count)
{
    assert(count == 1 && users && port.name && !strcmp(names[0], port.name));
    assert(users[0] == port.user);
    if (port_open) return ESP_ERR_INVALID_STATE;
    memset(&port, 0, sizeof(port));
    return ESP_OK;
}

static unsigned instance_pins;
static solar_os_contact_trust_t trust = SOLAR_OS_CONTACT_TRUST_TRUSTED;
static const uint8_t local_key[32] = {3, 5, 7};
static const uint8_t peer_key[32] = {2, 4, 6};
static const solar_os_messaging_provider_id_t mesh_provider = 3;
static const solar_os_endpoint_id_t peer_endpoint = 23;

esp_err_t solar_os_contacts_get_endpoint(solar_os_endpoint_id_t id, solar_os_endpoint_t *endpoint)
{
    if (id != peer_endpoint) return ESP_ERR_NOT_FOUND;
    *endpoint = (solar_os_endpoint_t){.id = id, .provider = mesh_provider, .trust = trust};
    endpoint->address.length = sizeof(peer_key);
    memcpy(endpoint->address.bytes, peer_key, sizeof(peer_key));
    return ESP_OK;
}

esp_err_t solar_os_contacts_find_endpoint(solar_os_messaging_provider_id_t provider,
    const uint8_t *address, size_t length, solar_os_endpoint_t *endpoint)
{
    if (provider != mesh_provider || length != sizeof(peer_key) || memcmp(address, peer_key, length))
        return ESP_ERR_NOT_FOUND;
    return solar_os_contacts_get_endpoint(peer_endpoint, endpoint);
}

bool solar_os_messaging_provider_is_class(solar_os_messaging_provider_id_t provider, const char *class_name)
{
    return provider == mesh_provider && !strcmp(class_name, "meshcore");
}

esp_err_t solar_os_messaging_instance_retain(solar_os_messaging_provider_id_t provider, void **lease)
{
    assert(provider == mesh_provider && lease && !instance_pins);
    *lease = &instance_pins;
    ++instance_pins;
    return ESP_OK;
}

void solar_os_messaging_instance_release(void *lease)
{
    if (lease) {
        assert(lease == &instance_pins && instance_pins == 1);
        --instance_pins;
    }
}

static void test_meshcore_stream(void)
{
    const solar_os_native_meshcore_stream_api_v1_t *mesh =
        solar_os_native_meshcore_stream_get(solar_os_native_link_get_service, sizeof(*mesh));
    assert(mesh && !mesh->transport_active(mesh_provider));
    assert(mesh->create("mesh", peer_endpoint) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !instance_pins);
    assert(!solar_os_meshcore_stream_transport_start(mesh_provider, local_key));
    assert(mesh->transport_active(mesh_provider));
    trust = SOLAR_OS_CONTACT_TRUST_BLOCKED;
    assert(mesh->create("mesh", peer_endpoint) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    trust = SOLAR_OS_CONTACT_TRUST_TRUSTED;
    assert(!mesh->create("mesh", peer_endpoint) && !instance_pins);
    assert(mesh->count() == 1 && port.name && !strcmp(port.name, "mesh"));
    assert(mesh->create("duplicate", peer_endpoint) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);

    solar_os_meshcore_stream_note_tx(peer_endpoint, ESP_ERR_TIMEOUT);
    solar_os_meshcore_stream_status_t status;
    assert(mesh->status_at(0, &status) && status.transport_last_error == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    assert(status.transport_errors == 1 && status.endpoint_id == peer_endpoint);
    assert(!mesh->status("mesh", &status) && !strcmp(status.stream.port, "mesh"));
    assert(!port.open(port.user));
    port_open = true;
    assert(mesh->remove("mesh") == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && mesh->count() == 1);
    assert(!port.close(port.user));
    port_open = false;

    assert(!solar_os_link_stream_remove("mesh"));
    assert(!solar_os_link_stream_create("_meshcore-3", "mesh", 1));
    assert(mesh->status("mesh", &status) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!mesh->remove("mesh") && port.name && !mesh->count());
    assert(!solar_os_link_stream_remove("mesh") && !port.name);
    assert(!mesh->create("mesh", peer_endpoint) && !instance_pins);
    assert(!mesh->remove("mesh") && !mesh->count());

    solar_os_meshcore_stream_transport_stop(mesh_provider);
    assert(!mesh->transport_active(mesh_provider) && !queues);
}

static void test_close_retry(void)
{
    assert(solar_os_link_create("radio", 1, SOLAR_OS_LINK_FRAME_MAX) == ESP_OK);
    destroy_while_receiving = true;
    solar_os_link_message_t discarded;
    assert(solar_os_link_receive("radio", &discarded, 5) == ESP_ERR_TIMEOUT);
    assert(solar_os_link_create("radio", 2, SOLAR_OS_LINK_FRAME_MAX) == ESP_ERR_INVALID_STATE);
    assert(queues == 2);
    assert(solar_os_link_destroy("radio") == ESP_OK && !queues);
    assert(solar_os_link_create("radio", 2, SOLAR_OS_LINK_FRAME_MAX) == ESP_OK);
    assert(solar_os_link_destroy("radio") == ESP_OK && !queues);
}

void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t memory_class, const char *tag)
{
    assert(memory_class == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && tag != NULL);
    return malloc(size);
}

void solar_os_memory_free(void *memory)
{
    free(memory);
}

static const solar_os_app_t app;
const solar_os_app_t *solar_os_application_descriptor(void)
{
    return &app;
}

static void verify_repeater_api(void)
{
    const solar_os_native_link_repeater_api_v1_t *api = solar_os_native_link_get_service(
        SOLAR_OS_NATIVE_LINK_REPEATER_SERVICE, SOLAR_OS_NATIVE_LINK_REPEATER_ABI, sizeof(*api));
    assert(api != NULL);
    solar_os_native_handle_t repeater;
    assert(api->open(500, &repeater) == SOLAR_OS_NATIVE_OK);
    assert(solar_os_native_handle_count() == 1);
    solar_os_link_message_t message = {
        .source = 101, .destination = 102, .sequence = 11, .type = SOLAR_OS_LINK_MESSAGE_TEXT,
    };
    solar_os_link_frame_t frame = {.len = 4U, .data = {1, 2, 3, 4}};
    solar_os_link_repeater_observe_result_t result;
    assert(api->observe(repeater, &message, &frame, 20, 0, &result) == SOLAR_OS_NATIVE_OK);
    assert(result == SOLAR_OS_LINK_REPEATER_QUEUED);
    assert(api->observe(repeater, &message, &frame, 21, 0, &result) == SOLAR_OS_NATIVE_OK);
    assert(result == SOLAR_OS_LINK_REPEATER_SUPPRESSED);
    solar_os_link_frame_t relay = {0};
    bool ready;
    assert(api->take_due(repeater, 99, &relay, &ready) == SOLAR_OS_NATIVE_OK && !ready);
    assert(api->take_due(repeater, 100, &relay, &ready) == SOLAR_OS_NATIVE_OK && ready);
    assert(relay.len == frame.len && !memcmp(relay.data, frame.data, frame.len));
    assert(api->note_transmit(repeater, true) == SOLAR_OS_NATIVE_OK);
    assert(api->note_invalid(repeater) == SOLAR_OS_NATIVE_OK);
    solar_os_link_repeater_status_t status;
    assert(api->status(repeater, &status) == SOLAR_OS_NATIVE_OK);
    assert(status.repeated == 1 && status.suppressed == 1 && status.invalid_frames == 1);

    void *state;
    assert(solar_os_native_handle_acquire_exclusive(repeater, SOLAR_OS_NATIVE_HANDLE_LINK_REPEATER,
        &state) == SOLAR_OS_NATIVE_OK);
    assert(api->close(repeater) == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(api->status(repeater, &status) == SOLAR_OS_NATIVE_ERROR_BUSY);
    solar_os_native_handle_release_exclusive(repeater);
    assert(api->reset(repeater, 101) == SOLAR_OS_NATIVE_OK);
    assert(api->observe(repeater, &message, &frame, 22, 0, &result) == SOLAR_OS_NATIVE_OK);
    assert(result == SOLAR_OS_LINK_REPEATER_IGNORED);
    assert(solar_os_native_handle_close_owner(&app) == SOLAR_OS_NATIVE_OK);
    assert(api->close(repeater) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(solar_os_native_handle_count() == 0);
}

int main(void)
{
    verify_repeater_api();
    test_close_retry();
    const solar_os_native_link_api_v1_t *api = solar_os_native_link_get_service(
        SOLAR_OS_NATIVE_LINK_SERVICE, SOLAR_OS_NATIVE_LINK_ABI, sizeof(*api));
    assert(api && api->struct_size == sizeof(*api));
    assert(!solar_os_native_link_get_service(SOLAR_OS_NATIVE_LINK_SERVICE, 2, sizeof(*api)));
    assert(api->create("a", 1, SOLAR_OS_LINK_FRAME_MAX) == SOLAR_OS_NATIVE_OK);
    assert(api->create("b", 2, SOLAR_OS_LINK_FRAME_MAX) == SOLAR_OS_NATIVE_OK);
    assert(api->count() == 2);
    const char text[] = "test";
    uint16_t sequence = 0;
    assert(!api->send("a", SOLAR_OS_LINK_MESSAGE_TEXT, 2, text, sizeof(text), &sequence));
    assert(sequence);
    solar_os_link_frame_t wire;
    assert(!api->take_tx("a", &wire, 0));
    solar_os_link_ingest_result_t result;
    assert(!api->ingest("b", wire.data, wire.len, &result) && result.accepted);
    solar_os_link_message_t message;
    assert(!api->receive("b", &message, 0));
    assert(message.source == 1 && message.destination == 2 && message.sequence == sequence);
    assert(message.payload_len == sizeof(text) && !memcmp(message.payload, text, sizeof(text)));
    wire.data[wire.len - 1] ^= 1U;
    assert(api->decode(wire.data, wire.len, &message) == SOLAR_OS_NATIVE_ERROR_FAILED);
    solar_os_link_status_t status;
    assert(!api->status("b", &status) && status.rx_messages == 1);
    assert(api->status_at(0, &status) && !strcmp(status.name, "a"));
    const solar_os_native_link_stream_api_v1_t *stream = solar_os_native_link_get_service(
        SOLAR_OS_NATIVE_LINK_STREAM_SERVICE, SOLAR_OS_NATIVE_LINK_STREAM_ABI, sizeof(*stream));
    assert(stream && stream->struct_size == sizeof(*stream));
    solar_os_link_stream_config_t config;
    stream->default_config(&config);
    assert(config.window_frames && config.retry_ms && config.port_label);
    assert(!stream->create("a", "byte-port", 2, NULL));
    assert(port.name && !strcmp(port.name, "byte-port") && stream->count() == 1);
    solar_os_link_stream_status_t stream_status;
    assert(!stream->status("byte-port", &stream_status) && stream_status.peer_id == 2);
    assert(stream->status_at(0, &stream_status) && !strcmp(stream_status.link, "a"));
    assert(!port.open(port.user));
    port_open = true;
    assert(stream->remove("byte-port") == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!port.close(port.user));
    port_open = false;
    assert(!stream->remove("byte-port") && !stream->count() && !port.name);

    assert(!api->destroy("a") && !api->destroy("b") && !queues);
    test_meshcore_stream();
    return 0;
}
