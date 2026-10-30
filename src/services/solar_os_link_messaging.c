#include "solar_os_link_messaging.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_random.h"
#include "solar_os_config.h"

#if SOLAR_OS_PACKAGE_SERVICE_MESSAGING

#include "solar_os_contacts.h"
#include "solar_os_log.h"
#include "solar_os_messaging.h"
#include "solar_os_memory.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdlib.h>

#define LINK_MESSAGING_ACK_TIMEOUT_MS 10000U

typedef struct link_messaging_state {
    struct link_messaging_state *next;
    solar_os_messaging_provider_id_t provider;
    struct { uint32_t version; bool enabled; char link[SOLAR_OS_LINK_NAME_MAX]; } config;
    solar_os_messaging_outbound_t outbound;
    bool active;
    bool inflight;
    bool transmitted;
    char link[SOLAR_OS_LINK_NAME_MAX];
    size_t payload_max;
    uint32_t request_id;
    uint32_t destination;
    uint16_t sequence;
    uint32_t session_epoch;
    uint32_t ack_deadline_ms;
} link_messaging_state_t;

static link_messaging_state_t *instances;
static SemaphoreHandle_t instances_lock;
static atomic_flag init_lock = ATOMIC_FLAG_INIT;
static bool initialized;
static const char *TAG = "link_messaging";

static uint64_t link_messaging_provider_key(link_messaging_state_t *state, uint32_t source,
                                            uint16_t sequence)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    const uint8_t identity[] = {
        (uint8_t)(state->session_epoch >> 24),
        (uint8_t)(state->session_epoch >> 16),
        (uint8_t)(state->session_epoch >> 8),
        (uint8_t)state->session_epoch,
        (uint8_t)(source >> 24),
        (uint8_t)(source >> 16),
        (uint8_t)(source >> 8),
        (uint8_t)source,
        (uint8_t)(sequence >> 8),
        (uint8_t)sequence,
    };
    for (size_t i = 0; i < sizeof(identity); i++) {
        hash ^= identity[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash != 0U ? hash : UINT64_MAX;
}

static void link_messaging_address_encode(uint32_t device_id, uint8_t address[4])
{
    address[0] = (uint8_t)(device_id >> 24);
    address[1] = (uint8_t)(device_id >> 16);
    address[2] = (uint8_t)(device_id >> 8);
    address[3] = (uint8_t)device_id;
}

static bool link_messaging_address_decode(const solar_os_messaging_address_t *address,
                                          uint32_t *device_id)
{
    if (address == NULL || device_id == NULL || address->length != 4U) {
        return false;
    }
    *device_id = ((uint32_t)address->bytes[0] << 24) | ((uint32_t)address->bytes[1] << 16) |
                 ((uint32_t)address->bytes[2] << 8) | (uint32_t)address->bytes[3];
    return *device_id != 0U && *device_id != SOLAR_OS_LINK_BROADCAST;
}

static void link_messaging_clear_inflight(link_messaging_state_t *state)
{
    state->inflight = false;
    state->transmitted = false;
    state->request_id = 0U;
    state->destination = 0U;
    state->sequence = 0U;
    state->ack_deadline_ms = 0U;
}

static void link_messaging_update_inflight(link_messaging_state_t *state, solar_os_delivery_state_t delivery, const char *error)
{
    if (!state->inflight) {
        return;
    }
    (void)solar_os_messaging_outbox_update(state->request_id, delivery, error);
    if (delivery == SOLAR_OS_DELIVERY_SENT || delivery == SOLAR_OS_DELIVERY_DELIVERED ||
        delivery == SOLAR_OS_DELIVERY_FAILED) {
        link_messaging_clear_inflight(state);
    }
}

static esp_err_t link_messaging_fail_outbound(uint32_t request_id, const char *error)
{
    return solar_os_messaging_outbox_update(request_id, SOLAR_OS_DELIVERY_FAILED, error);
}



static esp_err_t start_current(link_messaging_state_t *state)
{
    const char *link = state->config.link;
    if (link == NULL || link[0] == '\0' ||
        strnlen(link, SOLAR_OS_LINK_NAME_MAX) >= SOLAR_OS_LINK_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (state->active) {
        return ESP_ERR_INVALID_STATE;
    }

    solar_os_link_status_t status;
    esp_err_t error = solar_os_link_get_status(link, &status);
    if (error != ESP_OK) {
        return error;
    }
    char provider_key[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
    char title[SOLAR_OS_MESSAGING_TITLE_MAX];
    snprintf(provider_key, sizeof(provider_key), "b:%s", link);
    snprintf(title, sizeof(title), "%s broadcast", link);
    const solar_os_messaging_conversation_upsert_t conversation = {
        .provider = state->provider,
        .provider_key = provider_key,
        .kind = SOLAR_OS_CONVERSATION_BROADCAST,
        .title = title,
        .group_ref = SOLAR_OS_LINK_BROADCAST,
    };
    error = solar_os_messaging_conversation_upsert(&conversation, NULL);
    if (error != ESP_OK) {
        (void)solar_os_messaging_provider_set_status(state->provider, false, false,
                                                     error, "broadcast conversation unavailable");
        return error;
    }

    link_messaging_clear_inflight(state); state->active = false;
    do {
        state->session_epoch = esp_random();
    } while (state->session_epoch == 0U);
    state->active = true;
    state->payload_max =
        status.frame_mtu - SOLAR_OS_LINK_HEADER_SIZE - SOLAR_OS_LINK_CRC_SIZE;
    strlcpy(state->link, link, sizeof(state->link));

    char detail[SOLAR_OS_MESSAGING_ERROR_MAX];
    snprintf(detail, sizeof(detail), "%s local=%08" PRIx32 " payload=%u", link, status.local_id,
             (unsigned)state->payload_max);
    error = solar_os_messaging_provider_set_status(state->provider, true, true,
                                                   ESP_OK, detail);
    if (error != ESP_OK) {
        link_messaging_clear_inflight(state); state->active = false;
    } else (void)solar_os_messaging_join_update(state->provider, provider_key, SOLAR_OS_MESSAGING_JOIN_JOINED, NULL);
    return error;
}

static void stop_current(link_messaging_state_t *state)
{
    if (!state->active) {
        return;
    }
    if (state->inflight) {
        (void)solar_os_messaging_outbox_update(state->request_id, SOLAR_OS_DELIVERY_QUEUED,
                                               "Link stopped before delivery");
    }
    (void)solar_os_messaging_provider_set_status(state->provider, false, false,
                                                 ESP_OK, NULL);
    char key[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
    snprintf(key, sizeof(key), "b:%s", state->link);
    (void)solar_os_messaging_join_update(state->provider, key, SOLAR_OS_MESSAGING_JOIN_LEFT, NULL);
    link_messaging_clear_inflight(state); state->active = false;
}

static void process_current(link_messaging_state_t *state, uint32_t now_ms)
{
    if (!state->active) {
        return;
    }
    if (state->inflight) {
        if (state->transmitted && state->destination != SOLAR_OS_LINK_BROADCAST &&
            (int32_t)(now_ms - state->ack_deadline_ms) >= 0) {
            link_messaging_update_inflight(state, SOLAR_OS_DELIVERY_FAILED,
                                           "Link acknowledgement timed out");
        }
        return;
    }

    solar_os_messaging_outbound_t *outbound = &state->outbound;
    if (solar_os_messaging_outbox_peek(state->provider, outbound) != ESP_OK) {
        return;
    }

    solar_os_messaging_conversation_t conversation;
    if (solar_os_messaging_conversation_get(outbound->conversation_id, &conversation) != ESP_OK) {
        (void)link_messaging_fail_outbound(outbound->id, "Link conversation unavailable");
        return;
    }

    uint32_t destination = 0U;
    if (conversation.kind == SOLAR_OS_CONVERSATION_BROADCAST) {
        destination = SOLAR_OS_LINK_BROADCAST;
    } else if (conversation.kind == SOLAR_OS_CONVERSATION_DIRECT &&
               conversation.endpoint_id != SOLAR_OS_ENDPOINT_ID_NONE) {
        solar_os_endpoint_t endpoint;
        if (solar_os_contacts_get_endpoint(conversation.endpoint_id, &endpoint) != ESP_OK ||
            endpoint.provider != state->provider ||
            endpoint.trust == SOLAR_OS_CONTACT_TRUST_BLOCKED ||
            !link_messaging_address_decode(&endpoint.address, &destination)) {
            (void)link_messaging_fail_outbound(outbound->id, "Link contact unavailable or blocked");
            return;
        }
    } else {
        (void)link_messaging_fail_outbound(outbound->id,
                                           "Link supports direct and broadcast conversations");
        return;
    }

    const size_t body_len = strnlen(outbound->body, sizeof(outbound->body));
    if (body_len > state->payload_max) {
        char error[SOLAR_OS_MESSAGING_ERROR_MAX];
        snprintf(error, sizeof(error), "Link text exceeds %u-byte payload",
                 (unsigned)state->payload_max);
        (void)link_messaging_fail_outbound(outbound->id, error);
        return;
    }

    uint16_t sequence = 0U;
    const esp_err_t error = solar_os_link_send(state->link, SOLAR_OS_LINK_MESSAGE_TEXT,
                                               destination, outbound->body, body_len, &sequence);
    if (error == ESP_ERR_NO_MEM) {
        return;
    }
    if (error != ESP_OK) {
        (void)link_messaging_fail_outbound(outbound->id, esp_err_to_name(error));
        return;
    }

    state->inflight = true;
    state->request_id = outbound->id;
    state->destination = destination;
    state->sequence = sequence;
    if (solar_os_messaging_outbox_update(outbound->id, SOLAR_OS_DELIVERY_SENDING, NULL) != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "outbox request %" PRIu32 " disappeared after queueing", outbound->id);
        link_messaging_clear_inflight(state);
    }
}

static void transmit_current(link_messaging_state_t *state, const solar_os_link_frame_t *frame, esp_err_t result,
                                           uint32_t now_ms)
{
    if (!state->active || !state->inflight || frame == NULL) {
        return;
    }
    solar_os_link_message_t message;
    if (solar_os_link_decode(frame->data, frame->len, &message) != ESP_OK ||
        message.type != SOLAR_OS_LINK_MESSAGE_TEXT || message.sequence != state->sequence ||
        message.destination != state->destination) {
        return;
    }
    if (result != ESP_OK) {
        link_messaging_update_inflight(state, SOLAR_OS_DELIVERY_FAILED, esp_err_to_name(result));
        return;
    }
    if (message.destination == SOLAR_OS_LINK_BROADCAST) {
        link_messaging_update_inflight(state, SOLAR_OS_DELIVERY_SENT, NULL);
        return;
    }
    state->transmitted = true;
    state->ack_deadline_ms = now_ms + LINK_MESSAGING_ACK_TIMEOUT_MS;
}

static esp_err_t ingest_current(link_messaging_state_t *state, const solar_os_link_ingest_result_t *result,
                                              uint32_t now_ms)
{
    if (!state->active || result == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const solar_os_link_message_t *message = &result->message;
    if (result->acknowledgement) {
        if (state->inflight && state->destination == message->source &&
            state->sequence == message->sequence) {
            link_messaging_update_inflight(state, SOLAR_OS_DELIVERY_DELIVERED, NULL);
        }
        return ESP_OK;
    }
    if (!result->accepted || message->type != SOLAR_OS_LINK_MESSAGE_TEXT) {
        return ESP_OK;
    }
    if (message->payload_len > SOLAR_OS_LINK_PAYLOAD_MAX ||
        memchr(message->payload, '\0', message->payload_len) != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char body[SOLAR_OS_LINK_PAYLOAD_MAX + 1U];
    memcpy(body, message->payload, message->payload_len);
    body[message->payload_len] = '\0';

    uint8_t address[4];
    link_messaging_address_encode(message->source, address);
    char default_name[SOLAR_OS_CONTACT_NAME_MAX + 1U];
    snprintf(default_name, sizeof(default_name), "Link %08" PRIx32, message->source);
    solar_os_contact_id_t contact_id = SOLAR_OS_CONTACT_ID_NONE;
    solar_os_endpoint_id_t endpoint_id = SOLAR_OS_ENDPOINT_ID_NONE;
    esp_err_t error = solar_os_contacts_upsert_discovered(
        state->provider, address, sizeof(address), default_name,
        SOLAR_OS_ENDPOINT_CAP_DIRECT | SOLAR_OS_ENDPOINT_CAP_BROADCAST | SOLAR_OS_ENDPOINT_CAP_ACK,
        now_ms, state->link, strlen(state->link), &contact_id, &endpoint_id);
    if (error != ESP_OK) {
        return error;
    }

    solar_os_contact_t contact;
    solar_os_endpoint_t endpoint;
    if (solar_os_contacts_get(contact_id, &contact) != ESP_OK ||
        solar_os_contacts_get_endpoint(endpoint_id, &endpoint) != ESP_OK ||
        endpoint.trust == SOLAR_OS_CONTACT_TRUST_BLOCKED) {
        return ESP_ERR_INVALID_STATE;
    }

    char provider_key[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
    char title[SOLAR_OS_MESSAGING_TITLE_MAX];
    solar_os_conversation_kind_t kind;
    if (message->destination == SOLAR_OS_LINK_BROADCAST) {
        snprintf(provider_key, sizeof(provider_key), "b:%s", state->link);
        snprintf(title, sizeof(title), "%s broadcast", state->link);
        kind = SOLAR_OS_CONVERSATION_BROADCAST;
    } else {
        snprintf(provider_key, sizeof(provider_key), "direct:%" PRIu32, endpoint_id);
        strlcpy(title, contact.display_name, sizeof(title));
        kind = SOLAR_OS_CONVERSATION_DIRECT;
    }
    uint32_t security_flags = 0U;
    if (endpoint.trust == SOLAR_OS_CONTACT_TRUST_TRUSTED) {
        security_flags |= SOLAR_OS_SECURITY_PEER_TRUSTED;
    }
    const solar_os_messaging_inbound_t inbound = {
        .provider = state->provider,
        .conversation_key = provider_key,
        .conversation_kind = kind,
        .conversation_title = title,
        .contact_id = contact_id,
        .endpoint_id = endpoint_id,
        .group_ref = kind == SOLAR_OS_CONVERSATION_BROADCAST ? SOLAR_OS_LINK_BROADCAST : 0U,
        .provider_message_key =
            link_messaging_provider_key(state, message->source, message->sequence),
        .timestamp_ms = now_ms,
        .security_flags = security_flags,
        .sender = contact.display_name,
        .body = body,
    };
    return solar_os_messaging_publish_inbound(&inbound, NULL, NULL);
}

static link_messaging_state_t *find_link(const char *link)
{
    for (link_messaging_state_t *state = instances; state; state = state->next)
        if (link && !strcmp(state->config.link, link)) return state;
    return NULL;
}

static bool binding_valid(const char *link)
{
    if (!link || !link[0] || strnlen(link, SOLAR_OS_LINK_NAME_MAX) >= SOLAR_OS_LINK_NAME_MAX) return false;
    for (const unsigned char *p = (const unsigned char *)link; *p; p++)
        if (*p <= 32 || *p == 127 || *p == '/') return false;
    return true;
}

static esp_err_t save_enabled(link_messaging_state_t *state, bool enabled)
{
    bool before = state->config.enabled;
    state->config.enabled = enabled;
    esp_err_t error = solar_os_messaging_profile_save(state->provider, &state->config, sizeof(state->config));
    if (error != ESP_OK) state->config.enabled = before;
    return error;
}

static esp_err_t connect_instance(void *context, bool enabled)
{
    link_messaging_state_t *state = context;
    xSemaphoreTake(instances_lock, portMAX_DELAY);
    esp_err_t error = state->config.link[0] ? save_enabled(state, enabled) : ESP_ERR_INVALID_STATE;
    if (error == ESP_OK && !enabled) stop_current(state);
    xSemaphoreGive(instances_lock);
    return error;
}

static esp_err_t join_instance(void *context, const char *target,
    solar_os_conversation_id_t *conversation_id)
{
    link_messaging_state_t *state = context;
    xSemaphoreTake(instances_lock, portMAX_DELAY);
    char key[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
    snprintf(key, sizeof(key), "b:%s", state->config.link);
    char title[SOLAR_OS_MESSAGING_TITLE_MAX];
    snprintf(title, sizeof(title), "%s broadcast", state->config.link);
    const solar_os_messaging_conversation_upsert_t conversation = {
        .provider = state->provider, .provider_key = key,
        .kind = SOLAR_OS_CONVERSATION_BROADCAST, .title = title,
        .group_ref = SOLAR_OS_LINK_BROADCAST,
    };
    esp_err_t error = !state->config.link[0] ? ESP_ERR_INVALID_STATE :
        (strcmp(target, key) && strcmp(target, "broadcast") &&
         strcmp(target, state->config.link)) ? ESP_ERR_INVALID_ARG :
        solar_os_messaging_conversation_upsert(&conversation, conversation_id);
    if (error == ESP_OK) error = save_enabled(state, true);
    if (error == ESP_OK) (void)solar_os_messaging_join_update(state->provider, key,
        state->active ? SOLAR_OS_MESSAGING_JOIN_JOINED : SOLAR_OS_MESSAGING_JOIN_PENDING, NULL);
    xSemaphoreGive(instances_lock);
    return error;
}

static esp_err_t normalize_instance(void *context, const char *target, char *out, size_t size)
{
    link_messaging_state_t *state = context;
    xSemaphoreTake(instances_lock, portMAX_DELAY);
    int count = !strcmp(target, "broadcast") || !strcmp(target, state->config.link) ?
        snprintf(out, size, "b:%s", state->config.link) : snprintf(out, size, "%s", target);
    xSemaphoreGive(instances_lock);
    return count >= 0 && (size_t)count < size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t leave_instance(void *context, const char *target)
{
    link_messaging_state_t *state = context;
    esp_err_t error = connect_instance(context, false);
    if (error == ESP_OK) (void)solar_os_messaging_join_update(state->provider, target, SOLAR_OS_MESSAGING_JOIN_LEFT, NULL);
    return error;
}

static esp_err_t direct_instance(void *context, const char *target, solar_os_conversation_id_t *conversation)
{
    link_messaging_state_t *state = context;
    char *end;
    unsigned long id = strtoul(target, &end, 16);
    if (!target[0] || *end || !id || id >= UINT32_MAX) return ESP_ERR_INVALID_ARG;
    uint8_t address[4]; link_messaging_address_encode((uint32_t)id, address);
    solar_os_contact_id_t contact;
    solar_os_endpoint_id_t endpoint_id;
    esp_err_t error = solar_os_contacts_upsert_discovered(state->provider, address, sizeof(address), target,
        SOLAR_OS_ENDPOINT_CAP_DIRECT | SOLAR_OS_ENDPOINT_CAP_ACK, 0, NULL, 0, &contact, &endpoint_id);
    if (error != ESP_OK) return error;
    solar_os_endpoint_t endpoint;
    error = solar_os_contacts_get_endpoint(endpoint_id, &endpoint);
    if (error != ESP_OK) return error;
    if (endpoint.trust == SOLAR_OS_CONTACT_TRUST_BLOCKED) return ESP_ERR_INVALID_STATE;
    char key[SOLAR_OS_MESSAGING_PROVIDER_KEY_MAX];
    snprintf(key, sizeof(key), "direct:%" PRIu32, endpoint_id);
    solar_os_messaging_conversation_upsert_t request = {.provider = state->provider, .provider_key = key,
        .kind = SOLAR_OS_CONVERSATION_DIRECT, .title = target, .contact_id = contact, .endpoint_id = endpoint_id,
        .security_flags = endpoint.trust == SOLAR_OS_CONTACT_TRUST_TRUSTED ? SOLAR_OS_SECURITY_PEER_TRUSTED : 0};
    return solar_os_messaging_conversation_upsert(&request, conversation);
}

static esp_err_t attach_instance(solar_os_messaging_provider_id_t id)
{
    for (link_messaging_state_t *node = instances; node; node = node->next)
        if (node->provider == id) return ESP_OK;
    link_messaging_state_t *state = solar_os_memory_calloc(1, sizeof(*state), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "link.messaging");
    if (!state) return ESP_ERR_NO_MEM;
    state->provider = id; state->config.version = 1;
    esp_err_t error = solar_os_messaging_profile_load(id, &state->config, sizeof(state->config));
    if (id == SOLAR_OS_MESSAGING_PROVIDER_LINK && error == ESP_ERR_NOT_FOUND) error = ESP_OK;
    else if (error == ESP_OK && (state->config.version != 1 || !binding_valid(state->config.link))) error = ESP_ERR_INVALID_ARG;
    if (error == ESP_OK && state->config.link[0] && find_link(state->config.link)) error = ESP_ERR_INVALID_STATE;
    solar_os_messaging_provider_status_t status;
    if (error == ESP_OK) error = solar_os_messaging_provider_get_status(id, &status);
    if (error == ESP_OK) error = solar_os_messaging_instance_register(id, "link", status.name, state);
    if (error != ESP_OK) { solar_os_memory_free(state); return error; }
    state->next = instances; instances = state;
    return ESP_OK;
}

static esp_err_t create_locked(const char *name, const char *link, bool enabled, solar_os_messaging_provider_id_t *id)
{
    if (!binding_valid(link)) return ESP_ERR_INVALID_ARG;
    if (find_link(link)) return ESP_ERR_INVALID_STATE;
    link_messaging_state_t *state = solar_os_memory_calloc(1, sizeof(*state), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "link.messaging");
    if (!state) return ESP_ERR_NO_MEM;
    state->config.version = 1; state->config.enabled = enabled;
    strlcpy(state->config.link, link, sizeof(state->config.link));
    esp_err_t error = solar_os_messaging_instance_create("link", name, &state->config, sizeof(state->config), id);
    if (error == ESP_OK) {
        state->provider = *id;
        error = solar_os_messaging_instance_register(*id, "link", name, state);
    }
    if (error == ESP_OK) { state->next = instances; instances = state; }
    else solar_os_memory_free(state);
    return error;
}

static esp_err_t create_instance(const char *name, int argc, const char *const *argv, solar_os_messaging_provider_id_t *id)
{
    if (argc != 1) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(instances_lock, portMAX_DELAY);
    esp_err_t error = create_locked(name, argv[0], false, id);
    xSemaphoreGive(instances_lock); return error;
}

static esp_err_t configure_instance(void *context, int argc, const char *const *argv)
{
    if (argc != 1 || !binding_valid(argv[0])) return ESP_ERR_INVALID_ARG;
    link_messaging_state_t *state = context;
    xSemaphoreTake(instances_lock, portMAX_DELAY);
    link_messaging_state_t *other = find_link(argv[0]);
    esp_err_t error = other && other != state ? ESP_ERR_INVALID_STATE : ESP_OK;
    char before[SOLAR_OS_LINK_NAME_MAX]; strlcpy(before, state->config.link, sizeof(before));
    if (error == ESP_OK) {
        strlcpy(state->config.link, argv[0], sizeof(state->config.link));
        error = solar_os_messaging_profile_save(state->provider, &state->config, sizeof(state->config));
        if (error != ESP_OK) strlcpy(state->config.link, before, sizeof(state->config.link));
        else stop_current(state);
    }
    xSemaphoreGive(instances_lock); return error;
}

static esp_err_t remove_prepare(void *context)
{ (void)context; xSemaphoreTake(instances_lock, portMAX_DELAY); return ESP_OK; }
static void remove_finish(void *context, bool removed)
{
    link_messaging_state_t *state = context;
    if (removed) {
        stop_current(state);
        link_messaging_state_t **next = &instances;
        while (*next && *next != state) next = &(*next)->next;
        if (*next) *next = state->next;
        memset(state, 0, sizeof(*state)); solar_os_memory_free(state);
    }
    xSemaphoreGive(instances_lock);
}

static const solar_os_messaging_setting_t link_settings_fields[] = {
    {"Transport", "Existing Link transport name", SOLAR_OS_LINK_NAME_MAX, false, true},
};
static esp_err_t link_settings_read(void *context, unsigned form, solar_os_messaging_settings_t *settings)
{
    if (form) return ESP_ERR_NOT_FOUND;
    settings->label = "Link"; settings->fields = link_settings_fields; settings->count = 1;
    if (context) {
        link_messaging_state_t *state = context;
        xSemaphoreTake(instances_lock, portMAX_DELAY);
        settings->enabled = state->config.enabled;
        solar_os_messaging_settings_value(settings, 0, state->config.link);
        /* The transport is a resource of its radio-link / espnow-link job. */
        strlcpy(settings->worker, state->config.link, sizeof(settings->worker));
        settings->worker_is_resource = true;
        xSemaphoreGive(instances_lock);
    }
    return ESP_OK;
}
static esp_err_t link_settings_write(void *context, unsigned form, const char *name,
    const solar_os_messaging_settings_t *settings, solar_os_messaging_provider_id_t *id)
{
    if (form) return ESP_ERR_NOT_FOUND;
    const char *args[] = {settings->values[0]};
    if (!context) return create_instance(name, 1, args, id);
    *id = ((link_messaging_state_t *)context)->provider;
    return settings->changed[0] ? configure_instance(context, 1, args) : ESP_OK;
}
static const solar_os_messaging_provider_class_t link_class = {
    .name = "link", .capabilities = SOLAR_OS_ENDPOINT_CAP_DIRECT | SOLAR_OS_ENDPOINT_CAP_BROADCAST | SOLAR_OS_ENDPOINT_CAP_ACK,
    .create = create_instance, .configure = configure_instance, .connect = connect_instance,
    .normalize_target = normalize_instance, .join_open = join_instance, .leave = leave_instance, .direct_open = direct_instance,
    .remove_prepare = remove_prepare, .remove_finish = remove_finish,
    .settings_read = link_settings_read, .settings_write = link_settings_write,
};

esp_err_t solar_os_link_messaging_init(void)
{
    while (atomic_flag_test_and_set(&init_lock)) vTaskDelay(1);
    esp_err_t error = ESP_OK;
    if (!initialized) {
        error = solar_os_messaging_init();
        if (error == ESP_OK && !instances_lock) instances_lock = xSemaphoreCreateMutex();
        if (error == ESP_OK && !instances_lock) error = ESP_ERR_NO_MEM;
        if (error == ESP_OK) error = solar_os_messaging_class_register(&link_class);
        if (error == ESP_OK) {
            xSemaphoreTake(instances_lock, portMAX_DELAY);
            solar_os_messaging_provider_id_t cursor = 0;
            solar_os_messaging_provider_status_t status;
            while (solar_os_messaging_provider_next(&cursor, &status)) {
                if (strcmp(status.class_name, "link")) continue;
                error = attach_instance(cursor); if (error != ESP_OK) break;
            }
            initialized = error == ESP_OK;
            xSemaphoreGive(instances_lock);
        }
    }
    atomic_flag_clear(&init_lock); return error;
}

bool solar_os_link_messaging_available(void) { return true; }

esp_err_t solar_os_link_messaging_start(const char *link)
{
    if (!binding_valid(link)) return ESP_ERR_INVALID_ARG;
    esp_err_t error = solar_os_link_messaging_init(); if (error != ESP_OK) return error;
    xSemaphoreTake(instances_lock, portMAX_DELAY);
    link_messaging_state_t *state = find_link(link);
    if (!state) {
        for (state = instances; state && state->config.link[0]; state = state->next) {}
        if (state) {
            strlcpy(state->config.link, link, sizeof(state->config.link));
            error = save_enabled(state, true);
            if (error != ESP_OK) state->config.link[0] = 0;
        } else {
            char name[SOLAR_OS_MESSAGING_LABEL_MAX]; snprintf(name, sizeof(name), "link-%s", link);
            solar_os_messaging_provider_id_t id;
            error = create_locked(name, link, true, &id);
            state = find_link(link);
        }
    }
    if (error == ESP_OK && state && state->config.enabled) error = start_current(state);
    xSemaphoreGive(instances_lock); return error;
}

void solar_os_link_messaging_stop(const char *link)
{
    if (solar_os_link_messaging_init() != ESP_OK) return;
    xSemaphoreTake(instances_lock, portMAX_DELAY);
    link_messaging_state_t *state = find_link(link); if (state) stop_current(state);
    xSemaphoreGive(instances_lock);
}
void solar_os_link_messaging_process(const char *link, uint32_t now_ms)
{
    if (solar_os_link_messaging_init() != ESP_OK) return;
    xSemaphoreTake(instances_lock, portMAX_DELAY);
    link_messaging_state_t *state = find_link(link);
    if (state && state->config.enabled) {
        if (!state->active) (void)start_current(state);
        process_current(state, now_ms);
    }
    xSemaphoreGive(instances_lock);
}
void solar_os_link_messaging_note_transmit(const char *link, const solar_os_link_frame_t *frame, esp_err_t result, uint32_t now_ms)
{
    if (solar_os_link_messaging_init() != ESP_OK) return;
    xSemaphoreTake(instances_lock, portMAX_DELAY);
    link_messaging_state_t *state = find_link(link); if (state) transmit_current(state, frame, result, now_ms);
    xSemaphoreGive(instances_lock);
}
esp_err_t solar_os_link_messaging_note_ingest(const char *link, const solar_os_link_ingest_result_t *result, uint32_t now_ms)
{
    esp_err_t error = solar_os_link_messaging_init(); if (error != ESP_OK) return error;
    xSemaphoreTake(instances_lock, portMAX_DELAY);
    link_messaging_state_t *state = find_link(link);
    error = state && state->config.enabled ? ingest_current(state, result, now_ms) : ESP_OK;
    xSemaphoreGive(instances_lock); return error;
}

#else
esp_err_t solar_os_link_messaging_init(void) { return ESP_ERR_NOT_SUPPORTED; }

bool solar_os_link_messaging_available(void) { return false; }

esp_err_t solar_os_link_messaging_start(const char *link)
{
    (void)link;
    return ESP_ERR_NOT_SUPPORTED;
}

void solar_os_link_messaging_stop(const char *link) { (void)link; }

void solar_os_link_messaging_process(const char *link, uint32_t now_ms) { (void)link; (void)now_ms; }

void solar_os_link_messaging_note_transmit(const char *link, const solar_os_link_frame_t *frame, esp_err_t result,
                                           uint32_t now_ms)
{
    (void)link; (void)frame;
    (void)result;
    (void)now_ms;
}

esp_err_t solar_os_link_messaging_note_ingest(const char *link, const solar_os_link_ingest_result_t *result,
                                              uint32_t now_ms)
{
    (void)link; (void)result;
    (void)now_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
