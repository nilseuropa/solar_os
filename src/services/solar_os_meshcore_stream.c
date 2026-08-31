#include "solar_os_meshcore_stream.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_contacts.h"
#include "solar_os_link.h"

#define MESHCORE_STREAM_LINK_NAME "_meshcore"
#define MESHCORE_STREAM_ACK_DELAY_MS 250U
#define MESHCORE_STREAM_RETRY_MS 12000U
#define MESHCORE_STREAM_RETRY_JITTER_MS 4000U
#define MESHCORE_STREAM_OPEN_INTERVAL_MS 30000U
#define MESHCORE_STREAM_PEER_TIMEOUT_MS 180000U

typedef struct {
    bool active;
    char port[SOLAR_OS_PORT_NAME_MAX];
    solar_os_endpoint_id_t endpoint_id;
    uint32_t peer_id;
    uint8_t public_key[SOLAR_OS_MESHCORE_STREAM_PUBLIC_KEY_SIZE];
    uint32_t mesh_packets_sent;
    uint32_t mesh_packets_received;
    uint32_t transport_errors;
    esp_err_t transport_last_error;
} meshcore_stream_binding_t;

static SemaphoreHandle_t meshcore_stream_mutex;
static meshcore_stream_binding_t
    meshcore_stream_bindings[SOLAR_OS_MESHCORE_STREAM_MAX];
static bool meshcore_stream_transport_running;
static uint32_t meshcore_stream_local_peer_id;

static esp_err_t meshcore_stream_ensure_init(void)
{
    if (meshcore_stream_mutex != NULL) {
        return ESP_OK;
    }
    meshcore_stream_mutex = xSemaphoreCreateMutex();
    if (meshcore_stream_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return solar_os_link_stream_init();
}

static int meshcore_stream_find_port_locked(const char *port)
{
    for (size_t index = 0U;
         index < SOLAR_OS_MESHCORE_STREAM_MAX;
         index++) {
        if (meshcore_stream_bindings[index].active &&
            strcmp(meshcore_stream_bindings[index].port, port) == 0) {
            return (int)index;
        }
    }
    return -1;
}

static int meshcore_stream_find_endpoint_locked(
    solar_os_endpoint_id_t endpoint_id)
{
    for (size_t index = 0U;
         index < SOLAR_OS_MESHCORE_STREAM_MAX;
         index++) {
        if (meshcore_stream_bindings[index].active &&
            meshcore_stream_bindings[index].endpoint_id == endpoint_id) {
            return (int)index;
        }
    }
    return -1;
}

static int meshcore_stream_find_peer_locked(
    uint32_t peer_id,
    const uint8_t public_key[SOLAR_OS_MESHCORE_STREAM_PUBLIC_KEY_SIZE])
{
    for (size_t index = 0U;
         index < SOLAR_OS_MESHCORE_STREAM_MAX;
         index++) {
        const meshcore_stream_binding_t *binding =
            &meshcore_stream_bindings[index];
        if (binding->active && binding->peer_id == peer_id &&
            (public_key == NULL ||
             memcmp(binding->public_key,
                    public_key,
                    SOLAR_OS_MESHCORE_STREAM_PUBLIC_KEY_SIZE) == 0)) {
            return (int)index;
        }
    }
    return -1;
}

static bool meshcore_stream_endpoint_trusted(
    solar_os_endpoint_id_t endpoint_id,
    solar_os_endpoint_t *endpoint)
{
    return endpoint != NULL &&
           solar_os_contacts_get_endpoint(endpoint_id, endpoint) == ESP_OK &&
           endpoint->provider == SOLAR_OS_MESSAGING_PROVIDER_MESHCORE &&
           endpoint->trust == SOLAR_OS_CONTACT_TRUST_TRUSTED &&
           endpoint->address.length ==
               SOLAR_OS_MESHCORE_STREAM_PUBLIC_KEY_SIZE;
}

static void meshcore_stream_prune_removed(void)
{
    if (meshcore_stream_mutex == NULL) {
        return;
    }
    char ports[SOLAR_OS_MESHCORE_STREAM_MAX][SOLAR_OS_PORT_NAME_MAX] = {{0}};
    xSemaphoreTake(meshcore_stream_mutex, portMAX_DELAY);
    for (size_t index = 0U;
         index < SOLAR_OS_MESHCORE_STREAM_MAX;
         index++) {
        if (meshcore_stream_bindings[index].active) {
            strlcpy(ports[index],
                    meshcore_stream_bindings[index].port,
                    sizeof(ports[index]));
        }
    }
    xSemaphoreGive(meshcore_stream_mutex);

    for (size_t index = 0U;
         index < SOLAR_OS_MESHCORE_STREAM_MAX;
         index++) {
        solar_os_link_stream_status_t status;
        if (ports[index][0] == '\0' ||
            solar_os_link_stream_get_status(ports[index], &status) == ESP_OK) {
            continue;
        }
        xSemaphoreTake(meshcore_stream_mutex, portMAX_DELAY);
        const int found = meshcore_stream_find_port_locked(ports[index]);
        if (found >= 0) {
            memset(&meshcore_stream_bindings[found],
                   0,
                   sizeof(meshcore_stream_bindings[found]));
        }
        xSemaphoreGive(meshcore_stream_mutex);
    }
}

esp_err_t solar_os_meshcore_stream_init(void)
{
    return meshcore_stream_ensure_init();
}

esp_err_t solar_os_meshcore_stream_transport_start(
    const uint8_t public_key[SOLAR_OS_MESHCORE_STREAM_PUBLIC_KEY_SIZE])
{
    if (public_key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = meshcore_stream_ensure_init();
    if (error != ESP_OK) {
        return error;
    }
    const uint32_t local_peer_id =
        solar_os_meshcore_stream_peer_id(public_key);
    if (local_peer_id == 0U || local_peer_id == SOLAR_OS_LINK_BROADCAST) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(meshcore_stream_mutex, portMAX_DELAY);
    if (meshcore_stream_transport_running) {
        xSemaphoreGive(meshcore_stream_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreGive(meshcore_stream_mutex);

    error = solar_os_link_create(MESHCORE_STREAM_LINK_NAME,
                                 local_peer_id,
                                 SOLAR_OS_MESHCORE_STREAM_FRAME_MTU);
    if (error != ESP_OK) {
        return error;
    }
    xSemaphoreTake(meshcore_stream_mutex, portMAX_DELAY);
    meshcore_stream_local_peer_id = local_peer_id;
    meshcore_stream_transport_running = true;
    xSemaphoreGive(meshcore_stream_mutex);
    return ESP_OK;
}

void solar_os_meshcore_stream_transport_stop(void)
{
    if (meshcore_stream_mutex == NULL) {
        return;
    }
    xSemaphoreTake(meshcore_stream_mutex, portMAX_DELAY);
    const bool running = meshcore_stream_transport_running;
    meshcore_stream_transport_running = false;
    meshcore_stream_local_peer_id = 0U;
    xSemaphoreGive(meshcore_stream_mutex);
    if (!running) {
        return;
    }
    solar_os_link_stream_transport_stopped(MESHCORE_STREAM_LINK_NAME);
    (void)solar_os_link_destroy(MESHCORE_STREAM_LINK_NAME);
}

bool solar_os_meshcore_stream_transport_active(void)
{
    if (meshcore_stream_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(meshcore_stream_mutex, portMAX_DELAY);
    const bool active = meshcore_stream_transport_running;
    xSemaphoreGive(meshcore_stream_mutex);
    return active;
}

esp_err_t solar_os_meshcore_stream_create(
    const char *port,
    solar_os_endpoint_id_t endpoint_id)
{
    if (port == NULL || port[0] == '\0' ||
        endpoint_id == SOLAR_OS_ENDPOINT_ID_NONE) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = meshcore_stream_ensure_init();
    if (error != ESP_OK) {
        return error;
    }
    solar_os_endpoint_t endpoint;
    if (!meshcore_stream_endpoint_trusted(endpoint_id, &endpoint)) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t peer_id =
        solar_os_meshcore_stream_peer_id(endpoint.address.bytes);

    xSemaphoreTake(meshcore_stream_mutex, portMAX_DELAY);
    if (!meshcore_stream_transport_running) {
        xSemaphoreGive(meshcore_stream_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (peer_id == meshcore_stream_local_peer_id ||
        meshcore_stream_find_port_locked(port) >= 0 ||
        meshcore_stream_find_endpoint_locked(endpoint_id) >= 0 ||
        meshcore_stream_find_peer_locked(peer_id, NULL) >= 0) {
        xSemaphoreGive(meshcore_stream_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    int slot = -1;
    for (size_t index = 0U;
         index < SOLAR_OS_MESHCORE_STREAM_MAX;
         index++) {
        if (!meshcore_stream_bindings[index].active) {
            slot = (int)index;
            break;
        }
    }
    xSemaphoreGive(meshcore_stream_mutex);
    if (slot < 0) {
        return ESP_ERR_NO_MEM;
    }

    const solar_os_link_stream_config_t config = {
        .port_label = "MeshCore virtual serial",
        .acknowledgement_delay_ms = MESHCORE_STREAM_ACK_DELAY_MS,
        .retry_ms = MESHCORE_STREAM_RETRY_MS,
        .retry_jitter_ms = MESHCORE_STREAM_RETRY_JITTER_MS,
        .open_interval_ms = MESHCORE_STREAM_OPEN_INTERVAL_MS,
        .peer_timeout_ms = MESHCORE_STREAM_PEER_TIMEOUT_MS,
    };
    error = solar_os_link_stream_create_configured(
        MESHCORE_STREAM_LINK_NAME, port, peer_id, &config);
    if (error != ESP_OK) {
        return error;
    }

    xSemaphoreTake(meshcore_stream_mutex, portMAX_DELAY);
    if (!meshcore_stream_transport_running ||
        meshcore_stream_bindings[slot].active) {
        xSemaphoreGive(meshcore_stream_mutex);
        (void)solar_os_link_stream_remove(port);
        return ESP_ERR_INVALID_STATE;
    }
    meshcore_stream_binding_t *binding =
        &meshcore_stream_bindings[slot];
    memset(binding, 0, sizeof(*binding));
    binding->active = true;
    strlcpy(binding->port, port, sizeof(binding->port));
    binding->endpoint_id = endpoint_id;
    binding->peer_id = peer_id;
    memcpy(binding->public_key,
           endpoint.address.bytes,
           sizeof(binding->public_key));
    binding->transport_last_error = ESP_OK;
    xSemaphoreGive(meshcore_stream_mutex);
    return ESP_OK;
}

esp_err_t solar_os_meshcore_stream_remove(const char *port)
{
    if (port == NULL || port[0] == '\0' ||
        meshcore_stream_ensure_init() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(meshcore_stream_mutex, portMAX_DELAY);
    const int index = meshcore_stream_find_port_locked(port);
    xSemaphoreGive(meshcore_stream_mutex);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t error = solar_os_link_stream_remove(port);
    if (error != ESP_OK && error != ESP_ERR_NOT_FOUND) {
        return error;
    }
    xSemaphoreTake(meshcore_stream_mutex, portMAX_DELAY);
    const int current = meshcore_stream_find_port_locked(port);
    if (current >= 0) {
        memset(&meshcore_stream_bindings[current],
               0,
               sizeof(meshcore_stream_bindings[current]));
    }
    xSemaphoreGive(meshcore_stream_mutex);
    return ESP_OK;
}

size_t solar_os_meshcore_stream_count(void)
{
    if (meshcore_stream_ensure_init() != ESP_OK) {
        return 0U;
    }
    meshcore_stream_prune_removed();
    size_t count = 0U;
    xSemaphoreTake(meshcore_stream_mutex, portMAX_DELAY);
    for (size_t index = 0U;
         index < SOLAR_OS_MESHCORE_STREAM_MAX;
         index++) {
        if (meshcore_stream_bindings[index].active) {
            count++;
        }
    }
    xSemaphoreGive(meshcore_stream_mutex);
    return count;
}

static esp_err_t meshcore_stream_fill_status(
    const meshcore_stream_binding_t *binding,
    solar_os_meshcore_stream_status_t *status)
{
    if (binding == NULL || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(status, 0, sizeof(*status));
    status->endpoint_id = binding->endpoint_id;
    status->peer_id = binding->peer_id;
    status->mesh_packets_sent = binding->mesh_packets_sent;
    status->mesh_packets_received = binding->mesh_packets_received;
    status->transport_errors = binding->transport_errors;
    status->transport_last_error = binding->transport_last_error;
    return solar_os_link_stream_get_status(binding->port, &status->stream);
}

bool solar_os_meshcore_stream_get(size_t index,
                                  solar_os_meshcore_stream_status_t *status)
{
    if (status == NULL || meshcore_stream_ensure_init() != ESP_OK) {
        return false;
    }
    meshcore_stream_binding_t binding;
    bool found = false;
    size_t current = 0U;
    xSemaphoreTake(meshcore_stream_mutex, portMAX_DELAY);
    for (size_t slot = 0U;
         slot < SOLAR_OS_MESHCORE_STREAM_MAX;
         slot++) {
        if (!meshcore_stream_bindings[slot].active) {
            continue;
        }
        if (current++ == index) {
            binding = meshcore_stream_bindings[slot];
            found = true;
            break;
        }
    }
    xSemaphoreGive(meshcore_stream_mutex);
    return found && meshcore_stream_fill_status(&binding, status) == ESP_OK;
}

esp_err_t solar_os_meshcore_stream_get_status(
    const char *port,
    solar_os_meshcore_stream_status_t *status)
{
    if (port == NULL || status == NULL ||
        meshcore_stream_ensure_init() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    meshcore_stream_binding_t binding;
    xSemaphoreTake(meshcore_stream_mutex, portMAX_DELAY);
    const int index = meshcore_stream_find_port_locked(port);
    if (index < 0) {
        xSemaphoreGive(meshcore_stream_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    binding = meshcore_stream_bindings[index];
    xSemaphoreGive(meshcore_stream_mutex);
    return meshcore_stream_fill_status(&binding, status);
}

void solar_os_meshcore_stream_process(uint32_t now_ms)
{
    if (!solar_os_meshcore_stream_transport_active()) {
        return;
    }
    meshcore_stream_prune_removed();
    solar_os_link_stream_process(MESHCORE_STREAM_LINK_NAME, now_ms);
}

esp_err_t solar_os_meshcore_stream_take_tx(
    solar_os_endpoint_id_t *endpoint_id,
    uint8_t *envelope,
    size_t envelope_capacity,
    size_t *envelope_len)
{
    if (endpoint_id == NULL || envelope == NULL || envelope_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *endpoint_id = SOLAR_OS_ENDPOINT_ID_NONE;
    *envelope_len = 0U;
    if (!solar_os_meshcore_stream_transport_active()) {
        return ESP_ERR_INVALID_STATE;
    }
    solar_os_link_frame_t frame;
    esp_err_t error = solar_os_link_take_tx(
        MESHCORE_STREAM_LINK_NAME, &frame, 0U);
    if (error != ESP_OK) {
        return error;
    }
    solar_os_link_message_t message;
    error = solar_os_link_decode(frame.data, frame.len, &message);
    if (error != ESP_OK || message.type != SOLAR_OS_LINK_MESSAGE_STREAM) {
        return error != ESP_OK ? error : ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(meshcore_stream_mutex, portMAX_DELAY);
    const int index = meshcore_stream_find_peer_locked(
        message.destination, NULL);
    if (index >= 0 && message.source == meshcore_stream_local_peer_id) {
        *endpoint_id = meshcore_stream_bindings[index].endpoint_id;
    }
    xSemaphoreGive(meshcore_stream_mutex);
    if (*endpoint_id == SOLAR_OS_ENDPOINT_ID_NONE) {
        return ESP_ERR_NOT_FOUND;
    }
    return solar_os_meshcore_stream_envelope_encode(
        frame.data,
        frame.len,
        envelope,
        envelope_capacity,
        envelope_len);
}

void solar_os_meshcore_stream_note_tx(solar_os_endpoint_id_t endpoint_id,
                                      esp_err_t error)
{
    if (meshcore_stream_mutex == NULL) {
        return;
    }
    xSemaphoreTake(meshcore_stream_mutex, portMAX_DELAY);
    const int index = meshcore_stream_find_endpoint_locked(endpoint_id);
    if (index >= 0) {
        meshcore_stream_binding_t *binding =
            &meshcore_stream_bindings[index];
        binding->transport_last_error = error;
        if (error == ESP_OK) {
            binding->mesh_packets_sent++;
        } else {
            binding->transport_errors++;
        }
    }
    xSemaphoreGive(meshcore_stream_mutex);
}

esp_err_t solar_os_meshcore_stream_ingest(
    const uint8_t peer_public_key[SOLAR_OS_MESHCORE_STREAM_PUBLIC_KEY_SIZE],
    const uint8_t *envelope,
    size_t envelope_len,
    uint32_t now_ms)
{
    if (peer_public_key == NULL || envelope == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t *frame = NULL;
    size_t frame_len = 0U;
    esp_err_t error = solar_os_meshcore_stream_envelope_decode(
        envelope, envelope_len, &frame, &frame_len);
    if (error != ESP_OK) {
        return error;
    }
    solar_os_endpoint_t endpoint;
    if (solar_os_contacts_find_endpoint(
            SOLAR_OS_MESSAGING_PROVIDER_MESHCORE,
            peer_public_key,
            SOLAR_OS_MESHCORE_STREAM_PUBLIC_KEY_SIZE,
            &endpoint) != ESP_OK ||
        endpoint.trust != SOLAR_OS_CONTACT_TRUST_TRUSTED) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t peer_id =
        solar_os_meshcore_stream_peer_id(peer_public_key);

    xSemaphoreTake(meshcore_stream_mutex, portMAX_DELAY);
    const int index = meshcore_stream_find_peer_locked(
        peer_id, peer_public_key);
    const bool accepted = index >= 0 &&
        meshcore_stream_bindings[index].endpoint_id == endpoint.id;
    const uint32_t local_peer_id = meshcore_stream_local_peer_id;
    xSemaphoreGive(meshcore_stream_mutex);
    if (!accepted) {
        return ESP_ERR_NOT_FOUND;
    }

    solar_os_link_message_t decoded;
    error = solar_os_link_decode(frame, frame_len, &decoded);
    if (error != ESP_OK || decoded.type != SOLAR_OS_LINK_MESSAGE_STREAM ||
        decoded.source != peer_id || decoded.destination != local_peer_id) {
        error = error != ESP_OK ? error : ESP_ERR_INVALID_RESPONSE;
    } else {
        solar_os_link_ingest_result_t result;
        error = solar_os_link_ingest(MESHCORE_STREAM_LINK_NAME,
                                     frame,
                                     frame_len,
                                     &result);
        if (error == ESP_OK && result.accepted) {
            error = solar_os_link_stream_ingest(
                MESHCORE_STREAM_LINK_NAME, &result.message, now_ms);
        }
    }

    xSemaphoreTake(meshcore_stream_mutex, portMAX_DELAY);
    const int current = meshcore_stream_find_peer_locked(
        peer_id, peer_public_key);
    if (current >= 0) {
        meshcore_stream_binding_t *binding =
            &meshcore_stream_bindings[current];
        binding->transport_last_error = error;
        if (error == ESP_OK) {
            binding->mesh_packets_received++;
        } else {
            binding->transport_errors++;
        }
    }
    xSemaphoreGive(meshcore_stream_mutex);
    return error;
}
